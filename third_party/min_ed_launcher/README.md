`LICENSE` is the licence of min-ed-launcher (MIT, "Copyright (c) 2017 Chris"), exactly as it
stands at commit `139e4ff7ab9027925648fb27767cb87acdcf290a` of
https://github.com/rfvgyhn/min-ed-launcher.

The launcher's direct launch is a C++ port of parts of that project: `Steam.fs` (`steam_ticket.cpp`),
`MachineId.fs` (`machine_id.cpp`), `Api.fs` (`frontier_api.cpp`) and the launch path of `Product.fs`
and `App.fs` (`direct_launch.cpp`), all in `src\installer\`. Each ported file names its source file
and this commit in its header.

The launcher carries the licence as RCDATA 120 and prints it with `--licenses`; the core release zip
carries it as `MIN-ED-LAUNCHER-LICENSE.txt`.
