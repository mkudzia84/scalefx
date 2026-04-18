<!-- ScaleFX Studio — CodeMirror 6 editor wrapper. -->
<!-- YAML-only for now; add more languages when needed. Keeps the component -->
<!-- API close to a <textarea>: bind:value, optional readonly, fills parent. -->
<script lang="ts">
    import { onMount, onDestroy, createEventDispatcher } from 'svelte'
    import { EditorView, keymap, lineNumbers, highlightActiveLine, placeholder as cmPlaceholder } from '@codemirror/view'
    import { EditorState } from '@codemirror/state'
    import { defaultKeymap, history, historyKeymap, indentWithTab } from '@codemirror/commands'
    import { syntaxHighlighting, HighlightStyle, bracketMatching, indentOnInput, foldGutter, foldKeymap } from '@codemirror/language'
    import { yaml } from '@codemirror/lang-yaml'
    import { tags as t } from '@lezer/highlight'

    export let value: string = ''
    export let readonly: boolean = false
    export let placeholder: string = ''

    const dispatch = createEventDispatcher<{ change: string }>()

    let container: HTMLDivElement
    let view: EditorView | null = null
    let updatingFromProp = false

    // Tokyo-Night-ish palette, tuned to Studio's dark surface.
    const yamlHighlight = HighlightStyle.define([
        { tag: t.propertyName, color: '#7aa2f7' },
        { tag: t.string, color: '#9ece6a' },
        { tag: t.number, color: '#ff9e64' },
        { tag: t.bool, color: '#bb9af7' },
        { tag: t.atom, color: '#bb9af7' },
        { tag: t.null, color: '#f7768e' },
        { tag: t.keyword, color: '#bb9af7' },
        { tag: t.comment, color: '#565f89', fontStyle: 'italic' },
        { tag: t.operator, color: '#7dcfff' },
        { tag: t.meta, color: '#7dcfff' },
        { tag: t.punctuation, color: '#a9b1d6' },
    ])

    const cmTheme = EditorView.theme({
        '&': {
            height: '100%',
            fontSize: '12px',
            color: 'var(--text)',
            backgroundColor: 'var(--bg-base)',
        },
        '.cm-content': {
            fontFamily: 'var(--font-mono)',
            padding: '8px 0',
            caretColor: 'var(--text-bright)',
        },
        '.cm-scroller': { overflow: 'auto', fontFamily: 'var(--font-mono)' },
        '.cm-gutters': {
            backgroundColor: 'var(--bg-surface)',
            color: 'var(--text-dim)',
            border: 'none',
            borderRight: '1px solid var(--border)',
        },
        '.cm-activeLine': { backgroundColor: 'color-mix(in srgb, var(--accent) 8%, transparent)' },
        '.cm-activeLineGutter': { backgroundColor: 'color-mix(in srgb, var(--accent) 14%, transparent)' },
        '.cm-selectionBackground, &.cm-focused .cm-selectionBackground': {
            backgroundColor: 'color-mix(in srgb, var(--accent) 35%, transparent) !important',
        },
        '&.cm-focused': { outline: 'none' },
        '.cm-cursor, .cm-dropCursor': { borderLeftColor: 'var(--text-bright)' },
        '.cm-placeholder': { color: 'var(--text-dim)', fontStyle: 'italic' },
    }, { dark: true })

    onMount(() => {
        const extensions = [
            lineNumbers(),
            foldGutter(),
            history(),
            bracketMatching(),
            indentOnInput(),
            highlightActiveLine(),
            keymap.of([...defaultKeymap, ...historyKeymap, ...foldKeymap, indentWithTab]),
            yaml(),
            syntaxHighlighting(yamlHighlight),
            cmTheme,
            EditorState.readOnly.of(readonly),
            EditorView.updateListener.of((u) => {
                if (u.docChanged && !updatingFromProp) {
                    const text = u.state.doc.toString()
                    value = text
                    dispatch('change', text)
                }
            }),
        ]
        if (placeholder) extensions.push(cmPlaceholder(placeholder))

        view = new EditorView({
            state: EditorState.create({ doc: value, extensions }),
            parent: container,
        })
    })

    onDestroy(() => {
        view?.destroy()
        view = null
    })

    // External value changes → push into the editor.
    $: if (view && value !== view.state.doc.toString()) {
        updatingFromProp = true
        view.dispatch({
            changes: { from: 0, to: view.state.doc.length, insert: value },
        })
        updatingFromProp = false
    }
</script>

<div class="cm-host" bind:this={container}></div>

<style>
    .cm-host {
        flex: 1 1 auto;
        min-height: 160px;
        width: 100%;
        border: 1px solid var(--border);
        border-radius: 4px;
        overflow: hidden;
        display: flex;
        background: var(--bg-base);
    }
    .cm-host:focus-within { border-color: var(--accent); }
    .cm-host :global(.cm-editor) {
        flex: 1 1 auto;
        width: 100%;
        height: 100%;
    }
</style>
