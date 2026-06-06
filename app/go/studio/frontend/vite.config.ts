/// <reference types="vitest" />
import {defineConfig} from 'vite'
import {svelte} from '@sveltejs/vite-plugin-svelte'

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [svelte({hot: !process.env.VITEST})],
  test: {
    // jsdom so component tests + DOM-touching helpers work; pure-logic tests
    // ignore it. globals → describe/it/expect without imports.
    environment: 'jsdom',
    globals: true,
    include: ['src/**/*.{test,spec}.{ts,js}'],
  },
})
