# Other mods

## EDHM

EDHM installs itself as `d3d11.dll`. Only one file can have that name. The installer recognises the file already in that slot, renames it, writes one setting, and tells you what it did. ED-UP then passes each graphics call on to EDHM.

EDHM's uninstaller deletes `d3d11.dll`. After the rename, that name belongs to ED-UP, so the uninstall removes ED-UP and leaves EDHM under the renamed file. Press **Repair**. The installer recognises that state and puts both back.

The same rename by hand is one step in the [manual install](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/manual-install.md#running-alongside-edhm-by-hand): rename EDHM's file, put ED-UP's `d3d11.dll` in its place, and set the chain in `edvr.ini`.

## ReShade

Install ReShade the way its own instructions say, usually as `dxgi.dll`. ED-UP runs alongside it. Both sets of effects apply. ReShade needs no line in `edvr.ini`.

## Another mod that wants `d3d11.dll`

Mods built the way EDHM is built use that same filename. Copying ED-UP's file on top of theirs removes them, and the game reports no error. Run the installer and let it do the rename. If you place the files yourself, keep the other mod's file and point ED-UP at it, using the EDHM steps above.
