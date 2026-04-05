package main

// CoreApi provides core/protocol-level operations.
type CoreApi struct{ apiClient }

// NewCoreApi creates a CoreApi bound to the given connection.
func NewCoreApi(conn *Connection) *CoreApi {
	return &CoreApi{apiClient{conn}}
}

func (a *CoreApi) Init() ApiResult                      { return a.sendQuery(CmdInit(), CoreINIT_READY) }
func (a *CoreApi) Identify() ApiResult                  { return a.sendQuery(CmdIdentify(), CoreIDENTIFY, CoreINIT_READY) }
func (a *CoreApi) Shutdown() ApiResult                  { return a.sendACK(CmdShutdown()) }
func (a *CoreApi) Status() ApiResult                    { return a.sendQuery(CmdStatusReq(), CoreSTATUS) }
func (a *CoreApi) I2CScan() ApiResult                   { return a.sendQuery(CmdI2CScan(), CoreI2C_SCAN_RES) }
func (a *CoreApi) Keepalive() ApiResult                 { return a.sendACK(CmdKeepalive()) }
func (a *CoreApi) DiagHistory(count byte) ApiResult     { return a.sendACK(CmdDiagHistory(count)) }

// Reboot sends reboot command (fire-and-forget, no response expected).
func (a *CoreApi) Reboot() error { return a.conn.Send(CmdReboot()) }

// Bootsel sends BOOTSEL/DFU command (fire-and-forget).
func (a *CoreApi) Bootsel() error { return a.conn.Send(CmdBootsel()) }
