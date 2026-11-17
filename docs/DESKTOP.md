# Pixel Neko OS 26A230a Desktop Storage

The desktop uses ordinary files on the installed system volume:

```text
C:\
├── APPS\                 Protected system applications
├── Desktop\              Desktop contents
│   ├── File Management.pnk
│   ├── About This Machine.pnk
│   ├── GAME.pnk
│   └── .desktop-layout   Persistent icon coordinates
├── GAME\                 Default games
├── PDEVICE\              Protected loadable drivers
├── boot\                 Protected boot files
└── system\               Protected installed-system metadata
```

## Shortcut format

`.pnk` is a small UTF-8 text shortcut:

```ini
PNK1
kind=app
target=\APPS\notepad.pxe
title=Notepad
locked=0
```

`kind` may be `app`, `folder`, or `file`. A target beginning with `\` is
resolved against the current boot-system volume. The Desktop hides the `.pnk`
extension and draws a shortcut marker in the lower-right corner of the icon.

`File Management.pnk` and `About This Machine.pnk` have `locked=1` and are
also protected by the VFS. They cannot be deleted, renamed, or overwritten
through File Manager.

## Icon layout

`Desktop\.desktop-layout` stores every icon position as:

```text
PNK_LAYOUT1
740,30,File Management.pnk
668,88,Clock.pnk
```

Dragging an icon updates this file. `View > Rearrange Desktop` resets the
positions to the default grid. `Clean Up Desktop` no longer exists.

## Live mode

Live boots use the read-only `Desktop\` tree from the boot ISO/USB image.
They never resolve the Desktop through a local disk partition. Dragging,
creating, copying into, renaming, and deleting desktop items are rejected in
Live mode.

## Upgrade behavior

The graphical installer's keep-filesystem mode copies missing versioned files
into `GAME\`, `APPS\`, `PDEVICE\`, and related system folders, but preserves
existing `Desktop\` files and `.desktop-layout`. Legacy `APPS\puzzle.pxe` and
the `DEMO\` directory are migrated/removed on the first local boot of
26A230a. Missing locked desktop shortcuts are recreated automatically.
