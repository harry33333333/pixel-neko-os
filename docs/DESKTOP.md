# Pixel Neko OS 26A262a Desktop Storage

The desktop uses ordinary files on the installed system volume:

```text
C:\
├── APPS\                 Protected system applications
├── Desktop\              Desktop contents
│   ├── File Management.pnk
│   ├── About This Machine.pnk
│   ├── Pixel CPU Detect.pnk
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
Long desktop names wrap to two lines with at most seven English characters per
line. If the remaining text is still too long, the second line ends with `...`.

`File Management.pnk` and `About This Machine.pnk` have `locked=1` and are
also protected by the VFS. They cannot be deleted, renamed, or overwritten
through File Manager.

## Icon layout

`Desktop\.desktop-layout` stores every icon position as:

```text
PNK_LAYOUT2
r,72,30,File Management.pnk
a,640,86,Clock.pnk
```

`r` entries are automatically anchored to the top-right corner and are
reflowed whenever the screen resolution changes. The first value is the
right-side margin and the second is the vertical position. `a` entries are
manual absolute positions created by dragging an icon.

`View > Rearrange Desktop` switches every icon back to the top-right
resolution-aware layout. `Clean Up Desktop` no longer exists. Legacy
`PNK_LAYOUT1` files are migrated automatically.

The Desktop directory is scanned at startup, on explicit `Refresh`, and after
supported file-system mutations under `Desktop\`. These mutations only set a
pending flag; the compositor performs one refresh on its next update. The
directory is never polled every second, keeping blocking FAT/ATA I/O out of the
mouse, keyboard and audio hot path on older real-hardware chipsets.

Automatic icons use a 72-pixel right margin, 96-pixel column spacing and
72-pixel row spacing. Existing automatic `PNK_LAYOUT2` entries are normalized
to these values on load.

## Top-bar clock and wallpaper

Control Panel can toggle the date and seconds independently. Both are enabled
by default. The setting is stored in the existing `.pse` reserved flags so old
configuration slots remain readable.

`Restore Default Wallpaper` clears the custom image and returns the desktop to
the default Stipple 50% pattern.

## Live mode

Live boots use the read-only `Desktop\` tree from the boot ISO/USB image.
They never resolve the Desktop through a local disk partition. Dragging,
creating, copying into, renaming, and deleting desktop items are rejected in
Live mode.

Shortcut launches from Live media are resolved again through `APPS\` instead
of relying only on the current drive letter. Ring 0 system shortcuts use an
explicit trusted launch path.

On xHCI systems the event-ring segment table sets the required Change bit, so
Intel H610 controllers continue retiring HID transfer events after the ring
wraps instead of losing mouse input after 15–30 seconds.

## Upgrade behavior

The graphical installer's keep-filesystem mode copies missing versioned files
into `GAME\`, `APPS\`, `PDEVICE\`, and related system folders, but preserves
existing `Desktop\` files and `.desktop-layout`. Legacy `APPS\puzzle.pxe` and
the `DEMO\` directory are migrated/removed on the first local boot of
26A262a. Missing locked desktop shortcuts are recreated automatically, and
the new `Pixel CPU Detect.pnk` shortcut is added without overwriting user
desktop files or manual icon positions.
