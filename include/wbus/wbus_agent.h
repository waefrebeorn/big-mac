/* wbus_agent.h — headless agent / MCP-style command API for Big Mac (R017 G9).
 *
 * Lets an AI agent (or a named-pipe / stdin driver) drive the editor without
 * the GUI. This is the "agent API" gap: OpenCut exposes MCP; we expose a
 * simple line-oriented command protocol over stdin (or a FILE*), reusing the
 * real session / export / EDL / voice-polish APIs so there is no second code
 * path to keep in sync.
 *
 * Protocol (one command per line, "#" = comment):
 *   import <src> [proxy]        add a video clip (auto track) to the session
 *   split  <track> <clip> <t>   split a clip at timeline seconds t
 *   quality <0..1>              set the proxy/QoS dial (G1)
 *   polish <src> <out> <lufs>   voice-polish a WAV (two-pass G8) -> out WAV
 *   edl  <out.edl>              export CMX3600 EDL (G5)
 *   fcpxml <out.xml>            export Final Cut XML (G5)
 *   export <out.mp4> [srt]      render session -> mp4 (video + engine audio)
 *   quit                        stop reading
 * Returns 0 if all commands succeeded, -1 on any error.
 */

#ifndef WUBUS_WBUS_AGENT_H
#define WUBUS_WBUS_AGENT_H


#ifdef __cplusplus
extern "C" {
#endif
#include <stdio.h>
#include "wbus/wbus.h"

/* Run commands from `in` against session `s` + engine `e`. Returns 0/-1. */
int wb_agent_run(FILE *in, wb_session *s, wb_engine *e);

/* Run a single command line (already stripped). Returns 0/-1. */
int wb_agent_command(wb_session *s, wb_engine *e, const char *line);

/* R068: wire a host-created wb_perf into the agent bridge (the DAW calls
 * this once at startup; headless callers get NULL). */
struct wb_perf;
void wb_agent_set_perf(struct wb_perf *p);
struct wb_perf *wb_agent_get_perf(void);


#ifdef __cplusplus
}
#endif
#endif /* WUBUS_WBUS_AGENT_H */
