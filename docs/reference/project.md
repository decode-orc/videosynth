# `project`

Project identity. Optional — a project with no `project:` block is valid — but worth filling in, since these fields survive load/save round-trips and are what identify the file in the GUI.

```yaml
project:
  name: PalCavQuickStart
  version: "1.0"
  description: PAL CAV laserdisc with VITS and IEC 60856 biphase metadata
```

## Keys

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| `name` | string | No | A user-friendly project name |
| `version` | string | No | Project file version. Quote it so YAML tools treat it as a string |
| `description` | string | No | Free-text description |

No other key is accepted.

## Notes

`version` is the *project's* version, not videosynth's. It is carried through unchanged and is not interpreted; there is no schema migration keyed off it.

Quote the version. videosynth reads the value as the text written, but to other YAML tools an unquoted `1.10` is the number `1.1`.

None of these fields affect the generated signal. They are metadata for the human reading the file and for the GUI's window title and recent-files list.
