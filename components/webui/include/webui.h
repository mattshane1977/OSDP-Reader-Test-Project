#pragma once

/*
 * Web UI server.
 *
 * Endpoints:
 *   GET  /           — serves the embedded SPA (index.html)
 *   GET  /ws         — WebSocket upgrade; bidirectional JSON
 *
 * WebSocket protocol (host -> device):
 *   {"cmd":"mode","value":"read"}
 *   {"cmd":"mode","value":"write"}
 *   {"cmd":"mode","value":"idle"}
 *   {"cmd":"enroll","facility":1,"id":12345,"date":0}
 *   {"cmd":"key_set","key":"<32 hex chars>"}
 *   {"cmd":"reboot"}
 *
 * WebSocket protocol (device -> host):
 *   {"type":"mode","value":"read"}
 *   {"type":"card_read","ok":true,"uid":"...","facility":1,"id":99,"date":0}
 *   {"type":"read_fail","uid":"..."}
 *   {"type":"write_ok",...}  / {"type":"write_fail",...}
 *   {"type":"status","mode":"idle","driver":"pn532",...}     (sent on connect)
 *
 * Connection model: one WS client at a time. Second connection bumps the
 * first — fine for single-developer use, would need a fan-out for
 * production.
 */

void webui_start(void);
