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
monitor. The settings have no graphical editor yet. Restart the core after
editing them.

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
