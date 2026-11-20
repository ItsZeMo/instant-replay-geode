#pragma once
// Host tests only see FMOD through pointers held by AudioGrabber.
typedef int FMOD_RESULT;
namespace FMOD {
class System;
class Sound;
class Channel;
class ChannelGroup;
class DSP;
} // namespace FMOD
