# Design documents

One file per feature, `<slug>.md`, produced by the `/architect` skill from the matching
`docs/requirements/<slug>.md`. These files are public: they name components, data flow, files
and tests, but do not describe proprietary codec methods. Detail that cannot be public goes in
`docs/private/` (stripped from the public mirror) and is linked from here in one line.

The last section of each design, "Implementation brief", is the prompt handed to the coder agent.
