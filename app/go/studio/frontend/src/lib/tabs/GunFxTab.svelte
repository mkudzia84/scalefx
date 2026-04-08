<!-- ScaleFX Studio — GunFX Tab -->
<!-- Two-column layout: Left = Input Config + Trigger + Smoke. Right = Rate of Fire + Servo/Turret. -->
<script lang="ts">
    import { SendCommand } from '../../../wailsjs/go/main/App'

    export let boardLabel: string = 'GunFX'

    // ─── Input Channel Config ───
    let triggerChannel = 1
    let triggerThreshold_us = 1500
    let smokeChannel = 2
    let smokeThreshold_us = 1500
    let turretPitchChannel = 3
    let turretYawChannel = 4

    // ─── Trigger ───
    let triggerRpm = 200
    let triggerActive = false

    function triggerOn() {
        SendCommand(`trigger on ${triggerRpm}`)
        triggerActive = true
    }

    function triggerOff() {
        SendCommand('trigger off')
        triggerActive = false
    }

    // ─── Rate of Fire Bands ───
    interface RofBand {
        id: number
        name: string
        rpm: number
        threshold_us: number
        soundFile: string
    }

    let rofBands: RofBand[] = [
        { id: 1, name: 'Low', rpm: 200, threshold_us: 1200, soundFile: '/sounds/2A42/gun_200rpm.wav' },
        { id: 2, name: 'High', rpm: 550, threshold_us: 1700, soundFile: '/sounds/2A42/gun_550rpm.wav' },
    ]

    let nextRofId = 3
    let editRofName = ''
    let editRofRpm = 300
    let editRofThreshold_us = 1500
    let editRofSoundFile = ''
    let selectedRofId: number | null = null

    function addRofBand() {
        if (!editRofName) return
        rofBands = [...rofBands, {
            id: nextRofId++,
            name: editRofName,
            rpm: editRofRpm,
            threshold_us: editRofThreshold_us,
            soundFile: editRofSoundFile,
        }]
        editRofName = ''
        editRofRpm = 300
        editRofThreshold_us = 1500
        editRofSoundFile = ''
    }

    function removeRofBand(id: number) {
        rofBands = rofBands.filter(b => b.id !== id)
        if (selectedRofId === id) selectedRofId = null
    }

    function selectRofBand(id: number) {
        selectedRofId = selectedRofId === id ? null : id
        const band = rofBands.find(b => b.id === id)
        if (band) {
            editRofName = band.name
            editRofRpm = band.rpm
            editRofThreshold_us = band.threshold_us
            editRofSoundFile = band.soundFile
        }
    }

    function updateRofBand() {
        if (selectedRofId === null) return
        rofBands = rofBands.map(b =>
            b.id === selectedRofId
                ? { ...b, name: editRofName, rpm: editRofRpm, threshold_us: editRofThreshold_us, soundFile: editRofSoundFile }
                : b
        )
    }

    function testRofSound(file: string) {
        if (!file) return
        SendCommand(`audio.play 4 ${file} 80`)
    }

    // ─── Servos ───
    let servoId = 0
    let servoPulse_us = 1500
    let servoMin_us = 500
    let servoMax_us = 2500
    let servoSpeed = 0
    let servoAccel = 0
    let servoDecel = 0
    let recoilJerk = 0
    let recoilVariance = 0

    function servoSet() {
        SendCommand(`servo set ${servoId} ${servoPulse_us}`)
    }

    function servoConfig() {
        let cmd = `servo.config ${servoId} ${servoMin_us} ${servoMax_us}`
        if (servoSpeed || servoAccel || servoDecel) {
            cmd += ` ${servoSpeed} ${servoAccel} ${servoDecel}`
        }
        SendCommand(cmd)
    }

    function servoRecoil() {
        SendCommand(`servo.recoil ${servoId} ${recoilJerk} ${recoilVariance}`)
    }

    // ─── Smoke Generator ───
    let smokeHeaterOn = false
    let smokePulsing = false
    let smokeSpeed = 50
    let smokeHigh = 255
    let smokeLow = 0
    let smokePulse_ms = 500
    let smokeSpindown_ms = 3000
    let smokeHeaterLimit_mA = 0
    let smokeFanLimit_mA = 0

    function smokeHeatOn() {
        SendCommand('smoke heat on')
        smokeHeaterOn = true
    }

    function smokeHeatOff() {
        SendCommand('smoke heat off')
        smokeHeaterOn = false
    }

    function smokeApplyConfig() {
        SendCommand(`smoke.config pulsing=${smokePulsing ? 1 : 0}`)
        SendCommand(`smoke.config speed=${smokeSpeed}`)
        SendCommand(`smoke.config high=${smokeHigh}`)
        SendCommand(`smoke.config low=${smokeLow}`)
        SendCommand(`smoke.config pulse_ms=${smokePulse_ms}`)
        SendCommand(`smoke.config spindown_ms=${smokeSpindown_ms}`)
    }

    function smokeSetLimits() {
        if (smokeHeaterLimit_mA > 0) SendCommand(`smoke.limit heater ${smokeHeaterLimit_mA}`)
        if (smokeFanLimit_mA > 0) SendCommand(`smoke.limit fan ${smokeFanLimit_mA}`)
    }

    function smokeReset() {
        SendCommand('smoke.reset')
        smokeHeaterOn = false
    }

    // ─── Status ───
    function refreshStatus() { SendCommand('status') }
</script>

<div class="tab-content">
    <!-- Header -->
    <div class="board-header">
        <span class="board-icon" style="color: var(--error)">⌖</span>
        <div class="board-info">
            <h2>{boardLabel}</h2>
            <span class="board-type">gunfx — weapons system</span>
        </div>
        <button class="status-btn" on:click={refreshStatus} title="Refresh board status">Status</button>
    </div>

    <!-- ═══ Two-Column Layout ═══ -->
    <div class="two-col">

        <!-- ──── LEFT: Input Config + Trigger + Smoke ──── -->
        <div class="col">

            <!-- Input Channel Config -->
            <section class="card">
                <div class="card-header">
                    <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="4" y1="21" x2="4" y2="14"/><line x1="4" y1="10" x2="4" y2="3"/><line x1="12" y1="21" x2="12" y2="12"/><line x1="12" y1="8" x2="12" y2="3"/><line x1="20" y1="21" x2="20" y2="16"/><line x1="20" y1="12" x2="20" y2="3"/><line x1="1" y1="14" x2="7" y2="14"/><line x1="9" y1="8" x2="15" y2="8"/><line x1="17" y1="16" x2="23" y2="16"/></svg> Input Channels</h3>
                </div>
                <div class="form-grid cols-2">
                    <div class="form-field">
                        <span class="field-label">Trigger Channel</span>
                        <select bind:value={triggerChannel} class="field-input">
                            {#each Array(10) as _, i}
                                <option value={i + 1}>{i + 1}</option>
                            {/each}
                        </select>
                    </div>
                    <div class="form-field">
                        <span class="field-label">Trigger Threshold µs</span>
                        <input type="number" bind:value={triggerThreshold_us} class="field-input"
                               min="800" max="2200" step="10" />
                    </div>
                    <div class="form-field">
                        <span class="field-label">Smoke Channel</span>
                        <select bind:value={smokeChannel} class="field-input">
                            {#each Array(10) as _, i}
                                <option value={i + 1}>{i + 1}</option>
                            {/each}
                        </select>
                    </div>
                    <div class="form-field">
                        <span class="field-label">Smoke Threshold µs</span>
                        <input type="number" bind:value={smokeThreshold_us} class="field-input"
                               min="800" max="2200" step="10" />
                    </div>
                    <div class="form-field">
                        <span class="field-label">Turret Pitch Ch</span>
                        <select bind:value={turretPitchChannel} class="field-input">
                            {#each Array(10) as _, i}
                                <option value={i + 1}>{i + 1}</option>
                            {/each}
                        </select>
                    </div>
                    <div class="form-field">
                        <span class="field-label">Turret Yaw Ch</span>
                        <select bind:value={turretYawChannel} class="field-input">
                            {#each Array(10) as _, i}
                                <option value={i + 1}>{i + 1}</option>
                            {/each}
                        </select>
                    </div>
                </div>
            </section>

            <!-- Trigger -->
            <section class="card">
                <div class="card-header">
                    <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="12" y1="2" x2="12" y2="6"/><line x1="12" y1="18" x2="12" y2="22"/><line x1="2" y1="12" x2="6" y2="12"/><line x1="18" y1="12" x2="22" y2="12"/></svg> Trigger</h3>
                    <span class="state-badge" class:active={triggerActive}>
                        {triggerActive ? 'FIRING' : 'IDLE'}
                    </span>
                </div>
                <div class="control-row">
                    <span class="field-label">RPM</span>
                    <input type="number" bind:value={triggerRpm} class="field-input narrow"
                           min="50" max="2000" step="50" />
                    <button class="primary action-btn" on:click={triggerOn}>
                        <span class="btn-icon">⌖</span> Fire
                    </button>
                    <button class="action-btn danger" on:click={triggerOff}>
                        <span class="btn-icon">■</span> Cease Fire
                    </button>
                </div>
            </section>

            <!-- Smoke Generator -->
            <section class="card">
                <div class="card-header">
                    <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M8 19c0 1.7 1.3 3 3 3h2c1.7 0 3-1.3 3-3"/><path d="M12 2C6.5 2 2 6.5 2 12c0 3 1.3 5.7 3.4 7.5L7 21h10l1.6-1.5C20.7 17.7 22 15 22 12c0-5.5-4.5-10-10-10z"/><line x1="12" y1="2" x2="12" y2="9"/></svg> Smoke Generator</h3>
                    <span class="state-badge" class:active={smokeHeaterOn}>
                        {smokeHeaterOn ? 'HEATING' : 'OFF'}
                    </span>
                </div>
                <div class="control-row" style="margin-bottom: 12px">
                    <button class="primary action-btn" on:click={smokeHeatOn}>
                        <span class="btn-icon">🔥</span> Heat On
                    </button>
                    <button class="action-btn danger" on:click={smokeHeatOff}>
                        <span class="btn-icon">■</span> Heat Off
                    </button>
                    <button class="small danger" on:click={smokeReset}>Reset</button>
                </div>
                <div class="subsection">
                    <h4>Configuration</h4>
                    <div class="form-grid cols-3">
                        <div class="form-field">
                            <span class="field-label">Pulsing</span>
                            <label class="toggle">
                                <input type="checkbox" bind:checked={smokePulsing} />
                                <span class="toggle-text">{smokePulsing ? 'On' : 'Off'}</span>
                            </label>
                        </div>
                        <div class="form-field">
                            <span class="field-label">Speed</span>
                            <input type="number" bind:value={smokeSpeed} class="field-input" min="0" max="255" />
                        </div>
                        <div class="form-field">
                            <span class="field-label">High</span>
                            <input type="number" bind:value={smokeHigh} class="field-input" min="0" max="255" />
                        </div>
                        <div class="form-field">
                            <span class="field-label">Low</span>
                            <input type="number" bind:value={smokeLow} class="field-input" min="0" max="255" />
                        </div>
                        <div class="form-field">
                            <span class="field-label">Pulse ms</span>
                            <input type="number" bind:value={smokePulse_ms} class="field-input" min="10" max="5000" />
                        </div>
                        <div class="form-field">
                            <span class="field-label">Spindown ms</span>
                            <input type="number" bind:value={smokeSpindown_ms} class="field-input" min="0" max="10000" />
                        </div>
                    </div>
                    <div class="control-row" style="margin-top: 8px">
                        <button class="small" on:click={smokeApplyConfig}>Apply Config</button>
                    </div>
                </div>
                <div class="subsection">
                    <h4>Current Limits</h4>
                    <div class="form-row">
                        <span class="field-label">Heater mA</span>
                        <input type="number" bind:value={smokeHeaterLimit_mA} class="field-input narrow" min="0" max="5000" />
                        <span class="field-label">Fan mA</span>
                        <input type="number" bind:value={smokeFanLimit_mA} class="field-input narrow" min="0" max="5000" />
                        <button class="small" on:click={smokeSetLimits}>Set</button>
                    </div>
                </div>
            </section>
        </div>

        <!-- ──── RIGHT: Rate of Fire + Servo/Turret ──── -->
        <div class="col">

            <!-- Rate of Fire Bands -->
            <section class="card">
                <div class="card-header">
                    <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 20V10"/><path d="M18 20V4"/><path d="M6 20v-4"/></svg> Rate of Fire</h3>
                </div>

                <!-- Band list -->
                <div class="rof-list">
                    {#each rofBands as band (band.id)}
                        <div class="rof-item" class:selected={selectedRofId === band.id}
                             on:click={() => selectRofBand(band.id)}
                             on:keydown={(e) => e.key === 'Enter' && selectRofBand(band.id)}
                             role="button" tabindex="0">
                            <div class="rof-info">
                                <span class="rof-name">{band.name}</span>
                                <span class="rof-detail">{band.rpm} RPM — {band.threshold_us} µs</span>
                            </div>
                            <div class="rof-file" title={band.soundFile}>
                                {band.soundFile ? band.soundFile.split('/').pop() : '—'}
                            </div>
                            <div class="rof-actions">
                                <button class="small" on:click|stopPropagation={() => testRofSound(band.soundFile)}
                                        disabled={!band.soundFile}>▶</button>
                                <button class="small danger" on:click|stopPropagation={() => removeRofBand(band.id)}>✕</button>
                            </div>
                        </div>
                    {/each}
                    {#if rofBands.length === 0}
                        <div class="empty-state">No rate-of-fire bands defined.</div>
                    {/if}
                </div>

                <!-- Add/Edit form -->
                <div class="subsection">
                    <h4>{selectedRofId !== null ? 'Edit Band' : 'Add Band'}</h4>
                    <div class="form-grid cols-2">
                        <div class="form-field">
                            <span class="field-label">Name</span>
                            <input type="text" bind:value={editRofName} class="field-input" placeholder="e.g. Burst" />
                        </div>
                        <div class="form-field">
                            <span class="field-label">RPM</span>
                            <input type="number" bind:value={editRofRpm} class="field-input" min="50" max="3000" step="50" />
                        </div>
                        <div class="form-field">
                            <span class="field-label">Threshold µs</span>
                            <input type="number" bind:value={editRofThreshold_us} class="field-input" min="800" max="2200" step="10" />
                        </div>
                    </div>
                    <div class="form-row" style="margin-top: 6px">
                        <span class="field-label">Sound File</span>
                        <input type="text" bind:value={editRofSoundFile} class="field-input wide"
                               placeholder="/sounds/2A42/gun_200rpm.wav" />
                    </div>
                    <div class="control-row" style="margin-top: 8px">
                        {#if selectedRofId !== null}
                            <button class="primary small" on:click={updateRofBand}>Update</button>
                            <button class="small" on:click={() => { selectedRofId = null; editRofName = '' }}>Cancel</button>
                        {:else}
                            <button class="primary small" on:click={addRofBand} disabled={!editRofName}>Add Band</button>
                        {/if}
                    </div>
                </div>
            </section>

            <!-- Servo / Turret -->
            <section class="card">
                <div class="card-header">
                    <h3><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><polygon points="16.24 7.76 14.12 14.12 7.76 16.24 9.88 9.88 16.24 7.76"/></svg> Turret Servos</h3>
                </div>

                <div class="form-row">
                    <span class="field-label">Servo</span>
                    <select bind:value={servoId} class="field-input narrow">
                        <option value={0}>0 — Pitch</option>
                        <option value={1}>1 — Yaw</option>
                    </select>
                    <span class="field-label">Pulse µs</span>
                    <input type="number" bind:value={servoPulse_us} class="field-input narrow"
                           min="500" max="2500" step="10" />
                    <button class="primary small" on:click={servoSet}>Set</button>
                </div>

                <div class="slider-row">
                    <input type="range" bind:value={servoPulse_us} min="500" max="2500" step="10" class="slider wide" />
                    <span class="slider-val">{servoPulse_us} µs</span>
                </div>

                <div class="subsection">
                    <h4>Configuration</h4>
                    <div class="form-grid cols-3">
                        <div class="form-field">
                            <span class="field-label">Min µs</span>
                            <input type="number" bind:value={servoMin_us} class="field-input" min="500" max="2500" />
                        </div>
                        <div class="form-field">
                            <span class="field-label">Max µs</span>
                            <input type="number" bind:value={servoMax_us} class="field-input" min="500" max="2500" />
                        </div>
                        <div class="form-field">
                            <span class="field-label">Speed</span>
                            <input type="number" bind:value={servoSpeed} class="field-input" min="0" max="255" />
                        </div>
                        <div class="form-field">
                            <span class="field-label">Accel</span>
                            <input type="number" bind:value={servoAccel} class="field-input" min="0" max="255" />
                        </div>
                        <div class="form-field">
                            <span class="field-label">Decel</span>
                            <input type="number" bind:value={servoDecel} class="field-input" min="0" max="255" />
                        </div>
                    </div>
                    <div class="control-row" style="margin-top: 8px">
                        <button class="small" on:click={servoConfig}>Apply Config</button>
                    </div>
                </div>

                <div class="subsection">
                    <h4>Recoil Effect</h4>
                    <div class="form-row">
                        <span class="field-label">Jerk</span>
                        <input type="number" bind:value={recoilJerk} class="field-input narrow" min="0" max="255" />
                        <span class="field-label">Variance</span>
                        <input type="number" bind:value={recoilVariance} class="field-input narrow" min="0" max="255" />
                        <button class="small" on:click={servoRecoil}>Apply</button>
                    </div>
                </div>
            </section>
        </div>
    </div>
</div>

<style>
    /* GunFxTab-specific overrides — shared styles in style.css */

    .status-btn { margin-left: auto; }
    .col { gap: 16px; }

    .state-badge.active {
        background: color-mix(in srgb, var(--error) 15%, var(--bg-raised));
        color: var(--error);
        border-color: color-mix(in srgb, var(--error) 40%, transparent);
    }

    .slider-row { margin-bottom: 10px; }
    .slider-val { min-width: 64px; }

    .toggle-text {
        font-family: var(--font-mono);
        font-size: 12px;
        color: var(--text);
    }

    /* ─── Rate of Fire List ─── */
    .rof-list {
        display: flex;
        flex-direction: column;
        gap: 2px;
        max-height: 200px;
        overflow-y: auto;
        border: 1px solid var(--border);
        border-radius: 4px;
        background: var(--bg-input);
    }

    .rof-item {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 6px 10px;
        cursor: pointer;
        transition: background 0.1s;
    }

    .rof-item:hover { background: var(--bg-raised); }

    .rof-item.selected {
        background: color-mix(in srgb, var(--accent) 15%, var(--bg-raised));
        border-left: 2px solid var(--accent);
    }

    .rof-info {
        display: flex;
        flex-direction: column;
        min-width: 0;
        flex: 1;
    }

    .rof-name {
        font-size: 12px;
        font-weight: 600;
        color: var(--text-bright);
    }

    .rof-detail {
        font-size: 10px;
        font-family: var(--font-mono);
        color: var(--text-dim);
    }

    .rof-file {
        font-size: 10px;
        font-family: var(--font-mono);
        color: var(--text-dim);
        max-width: 100px;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .rof-actions {
        display: flex;
        gap: 4px;
        flex-shrink: 0;
    }
</style>
