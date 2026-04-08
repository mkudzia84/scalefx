<!-- ScaleFX Studio — About Dialog -->
<script lang="ts">
    import { showAboutDialog } from '../stores'
    import logoUrl from '../../assets/images/logo.jpg'

    function close() {
        $showAboutDialog = false
    }

    function handleKeydown(e: KeyboardEvent) {
        if (e.key === 'Escape' || e.key === 'Enter') close()
    }
</script>

<svelte:window on:keydown={handleKeydown} />

<div class="modal-backdrop" on:click|self={close}>
    <div class="modal about-modal">
        <img class="about-logo" src={logoUrl} alt="ScaleFX" />
        <div class="about-header">
            <h2>ScaleFX Studio</h2>
            <span class="about-version">v1.0.0</span>
        </div>

        <p class="about-desc">
            Visual configuration editor and diagnostics console
            for ScaleFX modular scale model effects controllers.
        </p>

        <div class="about-details">
            <div class="about-row">
                <span class="about-label">Platform</span>
                <span>Wails + Svelte</span>
            </div>
            <div class="about-row">
                <span class="about-label">Protocol</span>
                <span>Binary COBS, CRC-8, 6 Mbps</span>
            </div>
        </div>

        <div class="modal-actions">
            <div style="flex:1"></div>
            <button class="primary" on:click={close}>OK</button>
        </div>
    </div>
</div>

<style>
    .about-modal {
        text-align: left;
    }

    .about-logo {
        width: 100%;
        max-height: 140px;
        object-fit: cover;
        object-position: center;
        border-radius: 4px;
        margin-bottom: 14px;
        box-shadow: var(--logo-shadow);
    }

    .about-header {
        display: flex;
        align-items: baseline;
        gap: 10px;
        margin-bottom: 12px;
    }

    .about-header h2 {
        margin-bottom: 0;
    }

    .about-version {
        color: var(--text-dim);
        font-size: 13px;
    }

    .about-desc {
        color: var(--text-dim);
        line-height: 1.5;
        margin-bottom: 16px;
    }

    .about-details {
        border-top: 1px solid var(--border);
        padding-top: 12px;
    }

    .about-row {
        display: flex;
        gap: 16px;
        padding: 3px 0;
        font-size: 12px;
    }

    .about-label {
        width: 70px;
        color: var(--text-dim);
        flex-shrink: 0;
    }
</style>
