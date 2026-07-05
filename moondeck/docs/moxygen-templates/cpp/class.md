{{cleanAnchor refid name}}

## {{shortname name}}

```cpp
{{classSignature}}
```
{{#if (sourceLabel)}} <small>{{sourceLabel}}</small>{{/if}}
{{#if basecompoundref}}> **Inherits:** {{#each basecompoundref}}{{linkedName name refid}}{{#unless @last}}, {{/unless}}{{/each}}
{{/if}}

{{briefdescription}}

{{detaileddescription}}

{{#each inheritedMemberGroups}}
### Inherited from {{linkedName name refid}}

{{#each members}}- `{{kind}}` {{linkedName name refid}}{{#if (memberSummary this)}} — {{cell (memberSummary this)}}{{/if}}
{{/each}}

{{/each}}
{{! Public API only: skip the private/protected member sections (`section` is the raw
    Doxygen kind, a stabler discriminator than the English label). A technical
    reference documents the surface a caller uses, not internals. moxygen registers
    only 2-arg `eq`/`or`, so this is an explicit denylist of the eight private/
    protected kinds from moxygen's SECTION_LABELS. }}
{{#each filtered.sections}}
{{#unless (or (or (or (eq section "private-func") (eq section "private-static-func")) (or (eq section "private-attrib") (eq section "private-static-attrib"))) (or (or (eq section "private-slot") (eq section "protected-func")) (or (eq section "protected-attrib") (eq section "protected-slot"))))}}
### {{label}}

{{#each members}}
`{{#if returnTypeShort}}{{returnTypeShort}} {{/if}}{{signature}}`{{#if (memberSummary this)}}
: {{cell (memberSummary this)}}{{/if}}
{{#if enumvalue}}

| Value | Description |
|-------|-------------|
{{#each enumvalue}}| `{{name}}` | {{summary}} |
{{/each}}
{{/if}}

{{/each}}
{{/unless}}
{{/each}}
