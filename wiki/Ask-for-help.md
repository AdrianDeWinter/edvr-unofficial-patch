# Ask for help

The log usually shows the cause. A report with no log leaves very little to go on.

## Save logs

1. Close Elite. After a crash, confirm in Task Manager that `EliteDangerous64.exe` is gone.
2. Open the launcher.
3. Press **Save logs**.

On Windows the zip is written to the Desktop. Under Wine or Proton it is written to Downloads. The zip is also copied to the clipboard, so Ctrl+V attaches it in Discord or on a GitHub comment.

Inside it: the last session's logs, `edup_breadcrumbs.txt`, any `edup_FATAL.txt`, your settings, and the install record. A headset install also includes the runtime's settings file.

If Elite crashed, Save logs writes a second zip, `edvr-crash-dump-<time>.zip`. That file is a copy of the game's memory. Attach the log zip to a public issue. Send the dump in private, and only when someone asks for it.

On Linux, run `./ed-up-launcher.sh` and press **Save logs**. **Copy path** and **Show folder** appear beside it afterwards.

## Where to send it

A bug, with the zip attached, goes on [an issue](https://github.com/characterecho-sean/ed-unofficial-patch/issues/new/choose).

A setup question, or "is this normal", fits the [Discord](https://discord.gg/AWJ6djKKzr).

Say what you expected to see, whether you were in the headset or on a monitor, which store the copy came from (Steam, Epic, or Frontier), and the headset software if you use one.

## If the game never reaches the menu

`edup_breadcrumbs.txt` sits next to `EliteDangerous64.exe`. It is written as it goes, so a crash that loses the log still leaves this file. Attach it, and attach whatever is in `edup_logs`.
