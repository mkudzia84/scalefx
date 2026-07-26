import './style.css'
import { installDiagBridge } from './lib/diag'
import App from './App.svelte'

// Install the JS error capture (window.onerror / unhandledrejection /
// console.error wrapper) BEFORE the Svelte tree is instantiated — a throw
// during a child component's initial mount happens before App.svelte's
// onMount runs, so installing it there missed mount-time errors (a blanked
// UI with no logged FE.UNCAUGHT — 2026-07-25).
installDiagBridge()

const app = new App({
  target: document.getElementById('app')
})

export default app
