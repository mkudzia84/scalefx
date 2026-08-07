/// <reference types="vitest" />
import {defineConfig} from 'vite'
import {svelte} from '@sveltejs/vite-plugin-svelte'
import {fileURLToPath} from 'node:url'

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [svelte({hot: !process.env.VITEST})],
  resolve: {
    alias: {
      // Hardened store layer: one throwing subscriber must not poison
      // svelte/store's shared subscriber_queue and silently freeze every
      // store notification in the app (the 2026-07-26 "UI hangs after
      // connect" root cause).  TypeScript still type-checks against the
      // real svelte/store; only the runtime module is swapped.
      'svelte/store': fileURLToPath(new URL('./src/lib/safestore.ts', import.meta.url)),
    },
  },
  test: {
    // jsdom so component tests + DOM-touching helpers work; pure-logic tests
    // ignore it. globals → describe/it/expect without imports.
    environment: 'jsdom',
    globals: true,
    include: ['src/**/*.{test,spec}.{ts,js}'],
  },
})
