#ifndef WYZAR_AUDIO_EVENTS_H
#define WYZAR_AUDIO_EVENTS_H
// Producers publish their state under the existing IRQ guard, then notify the
// audio worker. The hook is installed when its task starts; pre-start events
// remain visible in the queue/mixer/startup predicates. No scheduling or I/O
// is performed by the producer itself.
namespace audio_events {
static void (*wake)()=nullptr;
static void notify(){if(wake)wake();}
}
#endif
