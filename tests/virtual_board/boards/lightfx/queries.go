// LightFX virtual board — typed query responses.
//
// Three query packets the firmware answers with structured data the
// CLI / Studio decode (see app/go/engine/handlers/lightfx/parsers.go):
//
//   LED_STATUS         (0x48) → LED_STATUS_RESP        (0x5B)
//   LED_SEQ_STATUS     (0x47) → LED_SEQ_STATUS_RESP    (0x5A)
//   LED_SEQ_QUEUE      (0x49) → LED_SEQ_QUEUE_RESP     (0x5D)
//
// Without these, every "View status" / "View sequence" panel in
// Studio sees a bare ACK and shows no data.

package lightfx

import (
	"scalefx/protocol"
	plfx "scalefx/protocol/lightfx"
	"scalefx/tests/virtual_board/server"
	"scalefx/tests/virtual_board/shared"
)

// handleLedStatus replies with LED_STATUS_RESP — a flat list of
// 4-byte tuples per channel: [ch:u8][brightness:u8][seqPlaying:u8][seqEventCount:u8].
func (b *Board) handleLedStatus(s server.Sender, tag byte) {
	b.st.mu.Lock()
	resp := make([]byte, 0, 4*8)
	for i, c := range b.st.channels {
		var playing byte
		if c.SeqPlaying {
			playing = 1
		}
		resp = append(resp,
			byte(i+1),       // 1-based channel id
			c.Brightness,    // 0..100
			playing,         // bool flag
			byte(len(c.SeqEvents)),
		)
	}
	b.st.mu.Unlock()
	s.Send(plfx.LedStatusResp, tag, resp)
}

// handleLedSeqStatus replies with LED_SEQ_STATUS_RESP for one channel:
// [ch:u8][playing:u8][eventCount:u8][currentIndex:u8][loops:u32LE][brightness:u8].
func (b *Board) handleLedSeqStatus(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, plfx.ErrInvalidChannel)
		return
	}
	ch := payload[0]
	if ch < 1 || ch > 8 {
		s.Nack(tag, plfx.ErrInvalidChannel)
		return
	}
	b.st.mu.Lock()
	c := &b.st.channels[ch-1]
	var playing byte
	if c.SeqPlaying {
		playing = 1
	}
	resp := make([]byte, 0, 9)
	resp = append(resp,
		ch, playing, byte(len(c.SeqEvents)), c.SeqIndex)
	resp = append(resp, protocol.U32LE(c.SeqLoops)...)
	resp = append(resp, c.Brightness)
	b.st.mu.Unlock()
	s.Send(plfx.LedSeqStatusResp, tag, resp)
}

// handleLedSeqQueue replies with LED_SEQ_QUEUE_RESP for one channel.
//
//   [ch:u8][count:u8][index:u8][playing:u8][brightness:u8]
//   then `count` × [type:u8][duration:u16LE][param1:u8]
//
// The "duration" field reports each event's total time — for FLASH /
// FADING / BEACON we report `cycle_ms`; for ON / OFF / FADE_IN /
// FADE_OUT we report `duration_ms`. param1 is the brightness or
// flash-pct depending on type, matching the firmware's protocol
// stuffing.
func (b *Board) handleLedSeqQueue(s server.Sender, tag byte, payload []byte) {
	if len(payload) < 1 {
		s.Nack(tag, plfx.ErrInvalidChannel)
		return
	}
	ch := payload[0]
	if ch < 1 || ch > 8 {
		s.Nack(tag, plfx.ErrInvalidChannel)
		return
	}
	b.st.mu.Lock()
	c := &b.st.channels[ch-1]
	count := byte(len(c.SeqEvents))
	if count > 24 {
		count = 24
	}
	var playing byte
	if c.SeqPlaying {
		playing = 1
	}
	resp := make([]byte, 0, 5+int(count)*4)
	resp = append(resp, ch, count, c.SeqIndex, playing, c.Brightness)
	for i := byte(0); i < count; i++ {
		ev := c.SeqEvents[i]
		dur, p1 := eventDurationParam(ev)
		resp = append(resp, ev.Type)
		resp = append(resp, protocol.U16LE(dur)...)
		resp = append(resp, p1)
	}
	b.st.mu.Unlock()
	s.Send(plfx.LedSeqQueueResp, tag, resp)
}

// eventDurationParam picks the most informative single field per event
// type for the queue summary. Mirrors the firmware's choice (used by
// `seq.queue` in the CLI):
//
//   ON / OFF / FADE_IN / FADE_OUT     → P1 (duration_ms)
//   FLASH / FADING / BEACON           → P1 (cycle_ms)
//
// param1 = brightness for ON/FLASH/FADE_IN/FADE_OUT, max-brightness
// for FADING, flash-pct for BEACON, 0 for OFF.
func eventDurationParam(ev shared.SeqEvent) (uint16, uint8) {
	switch ev.Type {
	case shared.EvtOn, shared.EvtFadeIn, shared.EvtFadeOut:
		return ev.P1, ev.P3
	case shared.EvtOff:
		return ev.P1, 0
	case shared.EvtFlash:
		return ev.P1, ev.P3
	case shared.EvtFading:
		return ev.P1, ev.P4 // max brightness
	case shared.EvtBeacon:
		return ev.P1, ev.P3 // flash_pct
	}
	return 0, 0
}
