# Windows monitor edge switching

On Windows, the server can use the exposed edge of an individual monitor for
an existing directional link. The Windows display layout and the network
protocol remain unchanged. Other platforms retain desktop edge switching.

Add one or more of the following keys to the `[server]` group of the **general
settings file**, not the external links configuration:

```ini
[server]
switchMonitorLeft=primary
```

The available keys are `switchMonitorLeft`, `switchMonitorRight`,
`switchMonitorTop` and `switchMonitorBottom`. Each accepts `primary` or the
Windows GDI display name, such as `\\.\DISPLAY1`. QSettings escapes backslashes
when saving INI files; using `primary` avoids manual escaping. Display numbers
can change after reconnecting displays. `primary` follows the Windows primary
monitor. The Monitors tab manages these selectors automatically. Restart the core after
editing them manually.

An empty or absent selector keeps the existing virtual desktop edge. A
configured selector replaces that side for all links on that side. If the
monitor is unavailable, or the selected edge is completely covered by another
local monitor, switching on that side is disabled instead of silently moving
the exit to another monitor. Only exposed edge segments are mapped; touching
edges shared with another local display preserve local movement.

Keep the usual directional links. For a notebook named `predator` to the left
of a desktop named `kyo`:

```text
section: links
    kyo:
        left = predator
    predator:
        right = kyo
end
```

The server projects the exposed monitor edge onto its virtual desktop edge.
Existing link ranges, switching delays and cursor locking continue through the
existing server path. Incoming points are mapped back to the selected monitor,
with an inset to avoid immediately switching out again. When an edge has several
exposed segments, their lengths are concatenated in top-to-bottom or left-to-right
order. There is no switch zone on the covered segments.

The monitor geometry is refreshed on Windows display change notifications,
including moves that leave the overall desktop bounds unchanged. The general
settings should be changed with the core stopped. Validate mixed DPI, display
hotplug, double-tap switching and both transition directions on the actual desk
before deploying this experimental feature.

The Windows backend also guards the first 500 ms after capturing the cursor.
Delayed hook positions near the old screen edge are discarded against a region
derived from the primary monitor, rather than the combined desktop. Discarding
an event resets the relative-motion baseline to the parking point so a later
warp echo cannot produce a reverse jump. Enable debug logging to see
`dropped stale motion after leave` when this guard rejects an event.

## Graphical monitor editor (Windows server)

Open Configure Server > Monitors, enable Use monitor layout, select a known
remote computer and click Add. Local monitors and their resolutions are detected with native Windows
coordinates. Select or drag any monitor, including local displays, and edit its
layout width, height, X and Y. Drag rectangles until their edges touch. Green edges indicate
bidirectional links; X/Y and resolution can also be entered precisely.

The remote desktop size is cached from the existing client information message
when it connects. Without a cached size the editor starts at 1920 x 1080; enter
the actual size before saving. One rectangle represents each remote computer's
entire desktop. Individual remote monitor discovery is not implemented.

Save validates overlaps, disconnected remote desktops and competing local
monitors for the same direction. The first implementation supports one selected
local monitor per direction. The editor derives fractional links from the
exposed contact segments and writes them atomically to `monitor-layout.conf`
beside the general configuration, retaining existing options and hotkeys. Cancel
does not save the layout. Disabling the layout returns to the Computers grid.

The layout is stored in `server/monitorLayout`; it contains configurable remote
rectangles and a local geometry snapshot to detect hardware changes, not a fixed
physical setup. Manual local placements are stored separately as overrides. Local monitors are detected again when
the dialog opens or Detect displays is clicked. Detection preserves manual overrides
for available display IDs. Use detected layout clears local overrides. Verify the layout and save again
after changing local monitor geometry. Windows GDI names may change after
reconnecting a display.

Incoming mouse movement carries its crossing direction to the Windows cursor
warp. This avoids inferring the destination from a one-pixel desktop border,
which failed when slow or fast motion overshot that border. Return positions are
restored to the selected monitor even when the incoming depth exceeds the jump
zone, and are clamped safely inside that monitor.

## Manual local layouts

Detected geometry and manual layout geometry are separate. The editor never
changes Windows resolution or display arrangement. Layout width/height and X/Y
control the representation and cross-computer contact ranges; local-to-local
cursor movement still follows Windows. A manually moved or scaled monitor maps
its contact ranges proportionally back to its actual exposed native edge.
Edges occupied by another local monitor in Windows remain local transitions.

No machine names, monitor count, resolutions or positions are fixed in production
code. Native detection enumerates the active Windows monitors. Remote desktop
resolution comes from client information when available and can be represented
with a manually configured layout size. The Computers tab remains available for
managing machines when monitor layout is enabled; their placement is configured
in Monitors. Arbitrary external configurations keep the original grid lock.
