# Risks

| # | Risk | Probability | Consequence | Mitigation |
|---|---|---|---|---|
| R1 | The ZBS243 registers (especially radio and sleep) cannot be found from the SoluM backup and experiments | Medium | Stops the tag track | Gate in phase 3. See "Plan B" below |
| R2 | The radio needs calibration or setup that is hard to see in disassembly | Medium | Poor range or no contact | Compare RSSI with the SoluM firmware; use the sniffer to see what the stock firmware transmits |
| R3 | Sleep current becomes too high | Medium | Short battery life | Phase 6 with proper measuring equipment; a longer interval (e.g. 5 min) is an acceptable emergency solution for prices that change daily |
| R4 | WiFi and 802.15.4 on the same C6 interfere with each other | Low–medium | Lost POLLs | Retry on the tag; otherwise the second C6 as a separate radio over UART |
| R5 | Code from other projects for the same hardware ends up in OpenESL by mistake, and brings its licence with it | Low | The project cannot be used freely | Write all code for this project, and record where each hardware fact comes from in [kildelogg.md](kildelogg.md) |
| R6 | Fault in the display controller version or OTP waveform gives poor contrast or ghosting | Low–medium | Faded or murky black, traces of the old image | Try the update modes in the datasheet |
| R7 | Motivation and time: the project takes longer than estimated | Medium | Never finished | Risk first (phase 3 early), small visible milestones |

## Plan B if the gate in phase 3 is not passed
1. **Change the tag model.** There are ESL tags with TI CC13xx/CC26xx or Nordic nRF52. They have public datasheets and SDKs with permissive licences. Then R1 and R2 almost disappear. The AP, the protocol and the HTTP API can be reused unchanged (CC26xx/nRF52 speak 802.15.4; CC13xx uses sub-GHz, which needs a different radio on the AP side).