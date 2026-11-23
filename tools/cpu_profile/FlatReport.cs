using System.Text.Json;

// The flat-profile report. A flat trace carries no VR frame markers: each owned Present
// emits event 5 (PresentFrame), and a frame is the interval between two consecutive
// Presents on one thread, [prevHookReturn, hookReturn]. That interval is cut into six
// regions that partition it exactly (.agents\plans\flat-cpu-markers.md, "Flat frame model"):
//
//   F1 before_first_command   prev -> firstCommand (zero when FirstCommandValid is clear)
//   F2 game_render            firstCommand -> hookEnter
//   F3 hook_before_present    hookEnter -> presentBegin
//   F4 present_call           presentBegin -> presentEnd (the monitor's presentWait)
//   F5 hook_after_present     presentEnd -> callbackEnd
//   F6 frame_start            callbackEnd -> hookReturn (the XeLL sleep)
//
// A spike is a covered frame well above the median cycle. Its dominant region is the one
// that grew most against that region's own median.

internal static class FlatReport
{
    public static readonly string[] RegionNames = ["F1", "F2", "F3", "F4", "F5", "F6"];
    public const double SpikeFactor = 1.5;
    public const double SpikeMinExcessMs = 2.0;
    public const double MinWaitMs = 0.2;
    private const int TopBlockingStacks = 50;
    private const int TopOtherThreads = 5;

    private sealed class Frame
    {
        public PresentFrameMarker Marker = null!;
        // Seven boundaries: prev, firstCommand, hookEnter, presentBegin, presentEnd,
        // callbackEnd, hookReturn. Region r runs from Bounds[r] to Bounds[r + 1].
        public double[] Bounds = new double[7];
        public StateDurations[] States = new StateDurations[6];
        public double CycleMs;
        public double MonitorCpuMs;
        public bool Spike;
        public List<SpanRow> Spans = [];

        public double RegionMs(int region) => (Bounds[region + 1] - Bounds[region]) / 1000.0;
    }

    private readonly record struct SpanRow(string Op, double BeginOffsetMs, double Ms, long Result);
    private readonly record struct WaitSeg(double Start, double End, long StartQpc, long EndQpc);
    private readonly record struct WaitRow(int Region, double OffsetMs, double Ms, int BlockingStackId,
                                           int WakerThread, int WakerProcess);

    public static Dictionary<string, object?> Build(string input, int pid, Collected data, string framesPath,
                                                    SymbolTable? symbols = null, Action<string>? progress = null)
    {
        if (data.Clocks.Count == 0) throw new InvalidDataException("trace has no valid EDVR Clock marker");
        if (data.KernelFirstQpc == long.MaxValue || data.KernelLastQpc == long.MinValue)
            throw new InvalidDataException("trace has no kernel scheduler events");
        var frequencies = data.Clocks.Select(c => c.QpcFrequency).Where(f => f > 0).Distinct().ToArray();
        if (frequencies.Length != 1)
            throw new InvalidDataException("Clock markers have missing or inconsistent QPC frequencies");
        var frequency = frequencies[0];
        double QpcUs(long qpc) => qpc * 1_000_000.0 / frequency;
        var traceStartUs = QpcUs(Report.CommonSchedulerStart(data.CpuSwitchRanges));
        var traceEndUs = QpcUs(data.KernelLastQpc);
        var clocksValid = data.Clocks.All(c =>
            Math.Abs((double)c.TimestampUs - c.QpcTicks * 1_000_000.0 / c.QpcFrequency) <= 2.0);
        var clocks = data.Clocks.OrderBy(c => c.TimestampUs).ToArray();

        var invalidReasons = new Dictionary<string, long>();
        var valid = new List<PresentFrameMarker>();
        var retainedInvalidFrames = 0;
        foreach (var marker in data.PresentFrames.OrderBy(frame => frame.TimestampUs))
        {
            var reason = Markers.FlatInvalidReason(marker);
            if (reason is null)
            {
                valid.Add(marker);
                continue;
            }
            Numeric.Increment(invalidReasons, reason);
            if ((double)marker.PrevHookReturnUs >= traceStartUs && (double)marker.HookReturnUs <= traceEndUs)
                retainedInvalidFrames++;
        }

        // Covered: admitted, inside the scheduler window, on a thread the scheduler timed.
        var frames = new List<Frame>();
        foreach (var marker in valid)
        {
            var caller = checked((int)marker.Thread);
            if ((double)marker.PrevHookReturnUs < traceStartUs || (double)marker.HookReturnUs > traceEndUs) continue;
            if (!data.Timelines.TryGetValue(caller, out var timeline)) continue;
            frames.Add(Analyze(marker, timeline, QpcUs));
        }

        var spansByThread = data.Spans.Where(span => span.TimestampUs >= span.BeginUs)
            .GroupBy(span => span.Thread)
            .ToDictionary(group => group.Key, group => group.OrderBy(span => span.BeginUs).ToArray());
        foreach (var frame in frames)
        {
            if (!spansByThread.TryGetValue(frame.Marker.Thread, out var spans)) continue;
            var start = frame.Marker.PrevHookReturnUs;
            var low = Numeric.LowerBound(spans.Length, i => spans[i].BeginUs >= start);
            for (var i = low; i < spans.Length && spans[i].BeginUs < frame.Marker.HookReturnUs; i++)
                frame.Spans.Add(new SpanRow(Markers.OperationName(spans[i].Operation),
                    ((double)spans[i].BeginUs - start) / 1000.0,
                    ((double)spans[i].TimestampUs - spans[i].BeginUs) / 1000.0, spans[i].Result));
        }

        var spanCounts = new Dictionary<string, long>();
        var retainedInvalidSpans = 0;
        foreach (var span in data.Spans)
        {
            if (span.TimestampUs < span.BeginUs)
            {
                if ((double)span.TimestampUs >= traceStartUs && (double)span.TimestampUs <= traceEndUs)
                    retainedInvalidSpans++;
                continue;
            }
            Numeric.Increment(spanCounts, Markers.OperationName(span.Operation));
        }

        long gaps = 0, duplicates = 0;
        foreach (var group in frames.GroupBy(frame => frame.Marker.Thread))
        {
            ulong? previous = null;
            foreach (var frame in group.OrderBy(f => f.Marker.Sequence))
            {
                var sequence = frame.Marker.Sequence;
                if (previous == sequence) duplicates++;
                else if (previous is not null && sequence > previous.Value + 1)
                    gaps += (long)(sequence - previous.Value - 1);
                previous = sequence;
            }
        }

        var coverageComplete = Report.CoverageComplete(data.EventsLost, data.MalformedMarkers,
            data.UnsupportedMarkers, (int)Math.Min(int.MaxValue, gaps), (int)Math.Min(int.MaxValue, duplicates),
            retainedInvalidSpans + retainedInvalidFrames, clocksValid, data.PresentFrames.Count, frames.Count,
            data.SwitchOut, data.SwitchIn, data.HasCallStacks,
            frames.SelectMany(frame => frame.States).Select(states => states.Unknown));

        // Spikes: the median cycle is the reference; the dominant region is the one whose
        // duration rose most above its own median.
        var cycleDist = Dist.Of(frames.Select(frame => frame.CycleMs).ToList());
        var median = cycleDist.P50;
        var regionDist = new Dist[RegionNames.Length];
        for (var r = 0; r < RegionNames.Length; r++)
            regionDist[r] = Dist.Of(frames.Select(frame => frame.RegionMs(r)).ToList());
        foreach (var frame in frames)
            frame.Spike = frame.CycleMs >= SpikeFactor * median && frame.CycleMs - median >= SpikeMinExcessMs;
        var spikes = frames.Where(frame => frame.Spike).ToList();

        var dominantCounts = RegionNames.ToDictionary(name => name, _ => 0L);
        var excessSums = new double[RegionNames.Length];
        var blockingGroups = new Dictionary<(string Kind, int StackId), long>();
        var waitsByThread = new Dictionary<int, WaitSeg[]>();
        var spikeFrames = new List<Dictionary<string, object?>>();
        foreach (var frame in spikes)
        {
            var dominant = DominantRegion(frame, regionDist);
            dominantCounts[RegionNames[dominant]]++;
            var waits = WaitsIn(frame, data, QpcUs, waitsByThread);
            foreach (var wait in waits)
            {
                if (wait.BlockingStackId >= 0)
                    Report.Bump(blockingGroups, (RegionNames[wait.Region] + "_wait", wait.BlockingStackId));
            }
            for (var r = 0; r < RegionNames.Length; r++) excessSums[r] += frame.RegionMs(r) - regionDist[r].P50;
            spikeFrames.Add(SpikeDetail(frame, data, clocks, QpcUs, regionDist, dominant, median, waits));
        }

        var spanMeans = SpanMeans(frames);
        var frameReports = new List<Dictionary<string, object?>>();
        foreach (var frame in frames) frameReports.Add(FrameDetail(frame));
        WriteFrames(framesPath, frameReports);

        var regions = new Dictionary<string, object?>();
        for (var r = 0; r < RegionNames.Length; r++)
        {
            var row = Numeric.JsonMs(regionDist[r]);
            row["runningUs"] = Numeric.Round(frames.Sum(frame => frame.States[r].Running));
            row["readyUs"] = Numeric.Round(frames.Sum(frame => frame.States[r].Ready));
            row["waitingUs"] = Numeric.Round(frames.Sum(frame => frame.States[r].Waiting));
            row["unknownUs"] = Numeric.Round(frames.Sum(frame => frame.States[r].Unknown));
            regions[RegionNames[r]] = row;
        }

        var seconds = (traceEndUs - traceStartUs) / 1_000_000.0;
        var dominantText = string.Join(",", dominantCounts.Where(pair => pair.Value > 0)
            .Select(pair => $"{pair.Key}:{pair.Value}"));
        progress?.Invoke($"flat: frames={data.PresentFrames.Count} valid={valid.Count} covered={frames.Count} " +
                         $"spikes={spikes.Count} dominant={dominantText}");

        var warnings = data.Stacks.Identity.Warning.Length == 0
            ? Array.Empty<string>() : new[] { "game build mismatch: " + data.Stacks.Identity.Warning };
        return new Dictionary<string, object?>
        {
            ["schemaVersion"] = 1,
            ["mode"] = "flat",
            ["warnings"] = warnings,
            ["input"] = input,
            ["pid"] = pid,
            ["providerGuid"] = Collector.EdvrProvider,
            ["qpcFrequency"] = frequency,
            ["schedulerTraceStartUs"] = Numeric.Round(traceStartUs),
            ["schedulerTraceEndUs"] = Numeric.Round(traceEndUs),
            ["schedulerTraceDurationMs"] = Numeric.Round((traceEndUs - traceStartUs) / 1000.0),
            ["eventsLost"] = data.EventsLost,
            ["hasCallStacks"] = data.HasCallStacks,
            ["clockCount"] = data.Clocks.Count,
            ["clocksValid"] = clocksValid,
            ["coverageComplete"] = coverageComplete,
            ["frameCount"] = data.PresentFrames.Count,
            ["presentFrameCount"] = data.PresentFrames.Count,
            ["validFrameCount"] = valid.Count,
            ["analyzedFrameCount"] = frames.Count,
            ["cycleDerivedFrameCount"] = valid.Count,
            ["cycleCoveredFrameCount"] = frames.Count,
            ["framesPath"] = framesPath,
            ["invalidFrameReasons"] = invalidReasons,
            ["retainedInvalidFrameCount"] = retainedInvalidFrames,
            ["sequenceGaps"] = gaps,
            ["sequenceDuplicates"] = duplicates,
            ["firstCommandKnownCount"] = frames.Count(frame =>
                (frame.Marker.Flags & Markers.FlatFirstCommandValid) != 0),
            ["spanCount"] = data.Spans.Count,
            ["spanCounts"] = spanCounts,
            ["switchOutCount"] = data.SwitchOut,
            ["switchInCount"] = data.SwitchIn,
            ["unsortedTimelineCount"] = data.UnsortedTimelines,
            ["callerThreads"] = data.CallerThreads.OrderBy(x => x).ToArray(),
            ["regionNames"] = RegionNames,
            ["regions"] = regions,
            ["cycleMs"] = Numeric.JsonMs(cycleDist),
            ["monitorCpuMs"] = Numeric.JsonMs(Dist.Of(frames.Select(frame => frame.MonitorCpuMs).ToList())),
            ["spikeRule"] = new Dictionary<string, object?>
            {
                ["factor"] = SpikeFactor,
                ["minExcessMs"] = SpikeMinExcessMs,
                ["medianCycleMs"] = Numeric.Round4(median),
            },
            ["spikes"] = new Dictionary<string, object?>
            {
                ["count"] = spikes.Count,
                ["perSecond"] = Numeric.Round4(seconds > 0 ? spikes.Count / seconds : 0),
                ["dominantRegionCounts"] = dominantCounts,
                ["meanExcessMsByRegion"] = RegionNames.Select((name, r) => (name, r))
                    .ToDictionary(item => item.name, item => (object?)Numeric.Round4(
                        spikes.Count > 0 ? excessSums[item.r] / spikes.Count : 0)),
                ["spanMeanMs"] = spanMeans,
            },
            ["spikeFrames"] = spikeFrames,
            ["topBlockingStacksInSpikes"] = Report.TopStacks(blockingGroups, data.Stacks, TopBlockingStacks),
            ["frames"] = frameReports,
            ["symbols"] = (symbols ?? SymbolTable.Off()).ToJson(),
        };
    }

    private static Frame Analyze(PresentFrameMarker marker, ThreadTimeline timeline, Func<long, double> qpcUs)
    {
        var prev = (double)marker.PrevHookReturnUs;
        // Without FirstCommandValid the first command is unknown: F1 is empty and F2 starts at prev.
        var firstCommand = (marker.Flags & Markers.FlatFirstCommandValid) != 0
            ? (double)marker.FirstCommandUs : prev;
        var frame = new Frame
        {
            Marker = marker,
            Bounds = new[]
            {
                prev, firstCommand, (double)marker.HookEnterUs, (double)marker.PresentBeginUs,
                (double)marker.PresentEndUs, (double)marker.CallbackEndUs, (double)marker.HookReturnUs,
            },
        };
        for (var r = 0; r < RegionNames.Length; r++)
            frame.States[r] = Numeric.Durations(timeline, frame.Bounds[r], frame.Bounds[r + 1], qpcUs);
        frame.CycleMs = (frame.Bounds[6] - prev) / 1000.0;
        frame.MonitorCpuMs = frame.CycleMs - frame.RegionMs(3);
        return frame;
    }

    private static int DominantRegion(Frame frame, Dist[] regionDist)
    {
        var best = 0;
        var bestExcess = double.NegativeInfinity;
        for (var r = 0; r < RegionNames.Length; r++)
        {
            var excess = frame.RegionMs(r) - regionDist[r].P50;
            if (excess > bestExcess)
            {
                bestExcess = excess;
                best = r;
            }
        }
        return best;
    }

    /// The caller's waits inside a frame, clipped to it. Each wait is named by the region
    /// its start falls in, the switch-out stack that began it, and the first ready event
    /// that ended it.
    private static List<WaitRow> WaitsIn(Frame frame, Collected data, Func<long, double> qpcUs,
                                         Dictionary<int, WaitSeg[]> cache)
    {
        var caller = checked((int)frame.Marker.Thread);
        if (!cache.TryGetValue(caller, out var segments))
            cache[caller] = segments = WaitSegments(data.Timelines[caller], qpcUs);
        var prev = frame.Bounds[0];
        var end = frame.Bounds[6];
        var rows = new List<WaitRow>();
        var low = Numeric.LowerBound(segments.Length, i => segments[i].End > prev);
        for (var i = low; i < segments.Length && segments[i].Start < end; i++)
        {
            var start = Math.Max(segments[i].Start, prev);
            var stop = Math.Min(segments[i].End, end);
            if (stop - start < MinWaitMs * 1000.0) continue;
            var region = 0;
            for (var r = 0; r < RegionNames.Length; r++)
                if (frame.Bounds[r] <= start) region = r;
            var startQpc = segments[i].StartQpc;
            var endQpc = segments[i].EndQpc;
            var blocking = -1;
            if (data.SwitchOutStacksByThread.TryGetValue(caller, out var outs))
            {
                var k = Numeric.LowerBound(outs.Count, j => outs[j].Qpc >= startQpc);
                if (k < outs.Count && outs[k].Qpc == startQpc) blocking = outs[k].StackId;
            }
            var wakerThread = -1;
            var wakerProcess = -1;
            if (data.ReadiesByThread.TryGetValue(caller, out var readies))
            {
                var k = Numeric.LowerBound(readies.Count, j => readies[j].Qpc >= startQpc);
                if (k < readies.Count && readies[k].Qpc <= endQpc)
                {
                    wakerThread = readies[k].WakerThread;
                    wakerProcess = readies[k].WakerProcess;
                }
            }
            rows.Add(new WaitRow(region, (start - prev) / 1000.0, (stop - start) / 1000.0, blocking,
                                 wakerThread, wakerProcess));
        }
        return rows;
    }

    private static WaitSeg[] WaitSegments(ThreadTimeline timeline, Func<long, double> qpcUs)
    {
        var list = new List<WaitSeg>();
        for (var i = 0; i < timeline.Count - 1; i++)
        {
            if (timeline.StateAt(i) != CpuState.Waiting) continue;
            list.Add(new WaitSeg(qpcUs(timeline.QpcAt(i)), qpcUs(timeline.QpcAt(i + 1)),
                                 timeline.QpcAt(i), timeline.QpcAt(i + 1)));
        }
        return list.ToArray();
    }

    private static Dictionary<string, object?> SpikeDetail(Frame frame, Collected data, ClockMarker[] clocks,
                                                           Func<long, double> qpcUs, Dist[] regionDist,
                                                           int dominant, double median, List<WaitRow> waits)
    {
        var caller = checked((int)frame.Marker.Thread);
        var excess = new Dictionary<string, object?>();
        for (var r = 0; r < RegionNames.Length; r++)
            excess[RegionNames[r]] = Numeric.Round4(frame.RegionMs(r) - regionDist[r].P50);
        var others = data.Timelines.Where(pair => pair.Key != caller)
            .Select(pair => (Thread: pair.Key, Running: Numeric.RunningUs(pair.Value, frame.Bounds[0],
                                                                          frame.Bounds[6], qpcUs)))
            .Where(item => item.Running > 0)
            .OrderByDescending(item => item.Running).ThenBy(item => item.Thread)
            .Take(TopOtherThreads)
            .Select(item => (object?)new Dictionary<string, object?>
            {
                ["threadId"] = item.Thread,
                ["runningUs"] = Numeric.Round(item.Running),
            }).ToList();
        return new Dictionary<string, object?>
        {
            ["sequence"] = frame.Marker.Sequence,
            ["thread"] = frame.Marker.Thread,
            ["utc"] = Markers.UtcText(Markers.Utc(clocks, frame.Marker.PrevHookReturnUs)),
            ["startUs"] = Numeric.Round(frame.Bounds[0]),
            ["cycleMs"] = Numeric.Round4(frame.CycleMs),
            ["cycleExcessMs"] = Numeric.Round4(frame.CycleMs - median),
            ["regionExcessMs"] = excess,
            ["dominantRegion"] = RegionNames[dominant],
            ["waits"] = waits.Select(wait => (object?)new Dictionary<string, object?>
            {
                ["region"] = RegionNames[wait.Region],
                ["offsetMs"] = Numeric.Round4(wait.OffsetMs),
                ["ms"] = Numeric.Round4(wait.Ms),
                ["blockingStackId"] = wait.BlockingStackId >= 0 ? (object)wait.BlockingStackId : null,
                ["wakerThread"] = wait.WakerThread >= 0 ? (object)wait.WakerThread : null,
                ["wakerProcess"] = wait.WakerProcess >= 0 ? (object)wait.WakerProcess : null,
            }).ToList(),
            ["topOtherThreads"] = others,
        };
    }

    /// Mean span duration per operation, split by whether the span began in a spike frame.
    private static Dictionary<string, object?> SpanMeans(List<Frame> frames)
    {
        var spike = new Dictionary<string, (double Sum, long N)>();
        var normal = new Dictionary<string, (double Sum, long N)>();
        foreach (var frame in frames)
        {
            var table = frame.Spike ? spike : normal;
            foreach (var span in frame.Spans)
            {
                var (sum, n) = table.GetValueOrDefault(span.Op);
                table[span.Op] = (sum + span.Ms, n + 1);
            }
        }
        var result = new Dictionary<string, object?>();
        foreach (var op in spike.Keys.Concat(normal.Keys).Distinct().OrderBy(name => name, StringComparer.Ordinal))
        {
            result[op] = new Dictionary<string, object?>
            {
                ["spike"] = spike.TryGetValue(op, out var s) ? (object)Numeric.Round4(s.Sum / s.N) : null,
                ["normal"] = normal.TryGetValue(op, out var n) ? (object)Numeric.Round4(n.Sum / n.N) : null,
            };
        }
        return result;
    }

    private static Dictionary<string, object?> FrameDetail(Frame frame)
    {
        var marker = frame.Marker;
        var regions = new Dictionary<string, object?>();
        for (var r = 0; r < RegionNames.Length; r++)
        {
            var states = frame.States[r];
            regions[RegionNames[r]] = new Dictionary<string, object?>
            {
                ["ms"] = Numeric.Round4(frame.RegionMs(r)),
                ["runningUs"] = Numeric.Round(states.Running),
                ["readyUs"] = Numeric.Round(states.Ready),
                ["waitingUs"] = Numeric.Round(states.Waiting),
                ["unknownUs"] = Numeric.Round(states.Unknown),
            };
        }
        return new Dictionary<string, object?>
        {
            ["sequence"] = marker.Sequence,
            ["thread"] = marker.Thread,
            ["startUs"] = Numeric.Round(frame.Bounds[0]),
            ["endUs"] = Numeric.Round(frame.Bounds[6]),
            ["cycleMs"] = Numeric.Round4(frame.CycleMs),
            ["monitorCpuMs"] = Numeric.Round4(frame.MonitorCpuMs),
            ["firstCommandKnown"] = (marker.Flags & Markers.FlatFirstCommandValid) != 0,
            ["generatedFrames"] = marker.GeneratedFrames,
            ["flags"] = marker.Flags,
            ["spike"] = frame.Spike,
            ["regions"] = regions,
            ["spans"] = frame.Spans.Select(span => (object?)new Dictionary<string, object?>
            {
                ["op"] = span.Op,
                ["beginOffsetMs"] = Numeric.Round4(span.BeginOffsetMs),
                ["ms"] = Numeric.Round4(span.Ms),
                ["result"] = span.Result,
            }).ToList(),
        };
    }

    private static void WriteFrames(string path, List<Dictionary<string, object?>> frames)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        using var writer = new StreamWriter(path, false);
        foreach (var frame in frames) writer.WriteLine(JsonSerializer.Serialize(frame));
    }
}
