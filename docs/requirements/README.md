# Requirements

One file per feature, `<slug>.md`, produced by the `/requirements` skill in conversation with the
user. These files are public: they describe what a change does and how it is judged done, not how
the codec achieves it. Each acceptance criterion should be something the tester agent can check by
building, running an exe, reading a trace line, or looking at a screenshot.

Workflow: `/requirements <name>` -> `/architect <slug>` -> coder agent -> tester agent -> reviewer agent -> sync to public.
