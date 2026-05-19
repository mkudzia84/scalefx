package client

import (
	"scalefx/protocol/alerts"
)

// Alerts is the master-side system-alert layer — short cue sounds
// (info/warning/error/critical) on the dedicated alert channel.
type Alerts struct{ c *Client }

// Re-exports.
type AlertStatus = alerts.Status

const (
	AlertInfo     = alerts.SeverityInfo
	AlertWarning  = alerts.SeverityWarning
	AlertError    = alerts.SeverityError
	AlertCritical = alerts.SeverityCritical
)

// Beep plays the preset sound for the given severity.  outputMask == 0
// means "use the severity preset's configured output mask".
func (a *Alerts) Beep(severity, outputMask byte) error {
	return a.c.sendExpectACK(alerts.CmdBeep(severity, outputMask))
}

// Info / Warning / Error / Critical are convenience methods.
func (a *Alerts) Info() error     { return a.Beep(AlertInfo, 0) }
func (a *Alerts) Warning() error  { return a.Beep(AlertWarning, 0) }
func (a *Alerts) Err() error      { return a.Beep(AlertError, 0) }
func (a *Alerts) Critical() error { return a.Beep(AlertCritical, 0) }

// Stop silences the alert channel.
func (a *Alerts) Stop() error { return a.c.sendExpectACK(alerts.CmdStop()) }

// Status reports whether the alert channel is currently playing and
// the last-fired severity (0xFF before any beep).
func (a *Alerts) Status() (AlertStatus, error) {
	resp, err := a.c.sendForResp(alerts.CmdStatusReq(), alerts.StatusResp)
	if err != nil {
		return AlertStatus{}, err
	}
	return alerts.DecodeStatus(resp.Payload)
}
