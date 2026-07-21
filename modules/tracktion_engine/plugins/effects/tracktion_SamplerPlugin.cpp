/*
    ,--.                     ,--.     ,--.  ,--.
  ,-'  '-.,--.--.,--,--.,---.|  |,-.,-'  '-.`--' ,---. ,--,--,      Copyright 2024
  '-.  .-'|  .--' ,-.  | .--'|     /'-.  .-',--.| .-. ||      \   Tracktion Software
    |  |  |  |  \ '-'  \ `--.|  \  \  |  |  |  |' '-' '|  ||  |       Corporation
    `---' `--'   `--`--'`---'`--'`--' `---' `--' `---' `--''--'    www.tracktion.com

    Tracktion Engine uses a GPL/commercial licence - see LICENCE.md for details.
*/

namespace tracktion::inline engine {

// this must be high enough for low freq sounds not to click
static constexpr int minimumSamplesToPlayWhenStopping = 8;
static constexpr int maximumSimultaneousNotes = 32;

// BSV-2185 instrumentation: diagnostic-only, no runtime-behaviour change.
// See SamplerPlugin::debugLog declaration (tracktion_SamplerPlugin.h) for what
// this is for.
SamplerPlugin::DebugLogFn SamplerPlugin::debugLog = nullptr;


struct SamplerPlugin::SampledNote   : public ReferenceCountedObject
{
public:
    SampledNote (int midiNote, int keyNote,
                 float velocity,
                 double sourceSampleRate,
                 double sampleRate,
                 int sampleDelayFromBufferStart,
                 const juce::AudioBuffer<float>& data,
                 int lengthInSamples,
                 float gainDb,
                 float pan,
                 bool openEnded_)
       : note (midiNote),
         offset (-sampleDelayFromBufferStart),
         audioData (data),
         openEnded (openEnded_)
    {
        resampler[0].reset();
        resampler[1].reset();

        const float volumeSliderPos = decibelsToVolumeFaderPosition (gainDb - (20.0f * (1.0f - velocity)));
        getGainsFromVolumeFaderPositionAndPan (volumeSliderPos, pan, getDefaultPanLaw(), gains[0], gains[1]);

        const double hz = juce::MidiMessage::getMidiNoteInHertz (midiNote);
        playbackRatio = hz / juce::MidiMessage::getMidiNoteInHertz (keyNote);
        playbackRatio *= sourceSampleRate / sampleRate;
        samplesLeftToPlay = playbackRatio > 0 ? (1 + (int) (lengthInSamples / playbackRatio)) : 0;
    }

    void addNextBlock (juce::AudioBuffer<float>& outBuffer, int startSamp, int numSamples)
    {
        jassert (! isFinished);

        if (offset < 0)
        {
            const int num = std::min (-offset, numSamples);
            startSamp += num;
            numSamples -= num;
            offset += num;
        }

        auto numSamps = std::min (numSamples, samplesLeftToPlay);

        if (numSamps > 0)
        {
            int numUsed = 0;

            for (int i = std::min (2, outBuffer.getNumChannels()); --i >= 0;)
            {
                numUsed = resampler[i]
                            .processAdding (playbackRatio,
                                            audioData.getReadPointer (std::min (i, audioData.getNumChannels() - 1), offset),
                                            outBuffer.getWritePointer (i, startSamp),
                                            numSamps,
                                            gains[i]);
            }

            offset += numUsed;
            samplesLeftToPlay -= numSamps;

            jassert (offset <= audioData.getNumSamples());
        }

        if (numSamples > numSamps && startFade > 0.0f)
        {
            startSamp += numSamps;
            numSamps = numSamples - numSamps;
            float endFade;

            if (numSamps > 100)
            {
                endFade = 0.0f;
                numSamps = 100;
            }
            else
            {
                endFade = std::max (0.0f, startFade - float (numSamps) * 0.01f);
            }

            const int numSampsNeeded = 2 + juce::roundToInt ((numSamps + 2) * playbackRatio);
            AudioScratchBuffer scratch (audioData.getNumChannels(), numSampsNeeded + 8);

            if (offset + numSampsNeeded < audioData.getNumSamples())
            {
                for (int i = scratch.buffer.getNumChannels(); --i >= 0;)
                    scratch.buffer.copyFrom (i, 0, audioData, i, offset, numSampsNeeded);
            }
            else
            {
                scratch.buffer.clear();
            }

            if (numSampsNeeded > 2)
                AudioFadeCurve::applyCrossfadeSection (scratch.buffer, 0, numSampsNeeded - 2,
                                                       AudioFadeCurve::linear, startFade, endFade);

            startFade = endFade;

            int numUsed = 0;

            for (int i = std::min (2, outBuffer.getNumChannels()); --i >= 0;)
                numUsed = resampler[i].processAdding (playbackRatio,
                                                      scratch.buffer.getReadPointer (std::min (i, scratch.buffer.getNumChannels() - 1)),
                                                      outBuffer.getWritePointer (i, startSamp),
                                                      numSamps, gains[i]);

            offset += numUsed;

            if (startFade <= 0.0f)
                isFinished = true;
        }
    }

    juce::LagrangeInterpolator resampler[2];
    int note;
    int offset, samplesLeftToPlay = 0;
    float gains[2];
    double playbackRatio = 1.0;
    const juce::AudioBuffer<float>& audioData;
    float lastVals[4] = { 0, 0, 0, 0 };
    float startFade = 1.0f;
    bool openEnded, isFinished = false;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampledNote)
};

//==============================================================================
SamplerPlugin::SamplerPlugin (PluginCreationInfo info)  : Plugin (info)
{
    triggerAsyncUpdate();
}

SamplerPlugin::~SamplerPlugin()
{
    notifyListenersOfDeletion();
}

const char* SamplerPlugin::xmlTypeName = "sampler";

void SamplerPlugin::valueTreeChanged()
{
    triggerAsyncUpdate();
    Plugin::valueTreeChanged();
}

void SamplerPlugin::flushPendingUpdates()
{
    TRACKTION_ASSERT_MESSAGE_THREAD
    handleUpdateNowIfNeeded();
}

void SamplerPlugin::handleAsyncUpdate()
{
    juce::OwnedArray<SamplerSound> newSounds;

    auto numSounds = state.getNumChildren();

    for (int i = 0; i < numSounds; ++i)
    {
        auto v = getSound (i);

        if (! v.hasType (IDs::SOUND))
            continue;

        const auto   src = v[IDs::source].toString();
        const double st  = static_cast<double> (v[IDs::startTime]);
        const double len = static_cast<double> (v[IDs::length]);

        // If an already-loaded sound has the same source and requested excerpt, move it
        // into the new list instead of re-reading the sample. This avoids reloading every
        // sound whenever the state changes, and a reused sound never touches its source
        // file again.
        SamplerSound* s = nullptr;

        for (int j = 0; j < soundList.size(); ++j)
        {
            auto* existing = soundList.getUnchecked (j);

            if (existing->sampleDataLoaded
                 && existing->source == src
                 && existing->requestedStartTime == st
                 && existing->requestedLength == len)
            {
                s = soundList.removeAndReturn (j);
                break;
            }
        }

        if (s == nullptr)
            s = new SamplerSound (*this, src, v[IDs::name], st, len, v[IDs::gainDb]);

        // Refresh the properties that don't require a reload (harmless for new sounds).
        s->name      = v[IDs::name];
        s->gainDb    = juce::jlimit (-48.0f, 48.0f, static_cast<float> (v[IDs::gainDb]));
        s->keyNote   = juce::jlimit (0, 127, static_cast<int> (v[IDs::keyNote]));
        s->minNote   = juce::jlimit (0, 127, static_cast<int> (v[IDs::minNote]));
        s->maxNote   = juce::jlimit (0, 127, static_cast<int> (v[IDs::maxNote]));
        s->pan       = juce::jlimit (-1.0f, 1.0f, static_cast<float> (v[IDs::pan]));
        s->openEnded = v[IDs::openEnded];

        newSounds.add (s);
    }

    {
        const juce::ScopedLock sl (lock);
        allNotesOff();
        soundList.swapWith (newSounds);
    }

    // newSounds now holds the previous sounds that weren't reused; clearing deletes them.
    newSounds.clear();
    changed();
}

void SamplerPlugin::initialise (const PluginInitialisationInfo&)
{
    const juce::ScopedLock sl (lock);
    allNotesOff();
}

void SamplerPlugin::deinitialise()
{
    allNotesOff();
}

//==============================================================================
void SamplerPlugin::playNotes (const juce::BigInteger& keysDown)
{
    // BSV-2185 instrumentation: time-to-acquire this lock, which the audio
    // thread also needs every callback (applyToBuffer) — priority-inversion
    // risk flagged by the Static Code Audit, never measured before.
    // [Review fix] The debugLog call MUST happen after `sl` releases the lock,
    // not while it's still held — logging inside the lock (vsnprintf + emit(),
    // a blocking fprintf(stderr) syscall on iOS) would extend exactly the
    // contention window this diagnostic exists to characterize. Measure the
    // wait, do the real work, let the nested scope release the lock, then log.
    const auto lockWaitStartMs = juce::Time::getMillisecondCounterHiRes();
    double lockWaitMs = 0.0;
    {
        const juce::ScopedLock sl (lock);
        lockWaitMs = juce::Time::getMillisecondCounterHiRes() - lockWaitStartMs;

        if (highlightedNotes != keysDown)
        {
            for (int i = playingNotes.size(); --i >= 0;)
                if ((! keysDown [playingNotes.getUnchecked(i)->note])
                     && highlightedNotes [playingNotes.getUnchecked(i)->note]
                     && ! playingNotes.getUnchecked(i)->openEnded)
                    playingNotes.getUnchecked(i)->samplesLeftToPlay = minimumSamplesToPlayWhenStopping;

            for (int note = 128; --note >= 0;)
            {
                if (keysDown [note] && ! highlightedNotes [note])
                {
                    for (auto ss : soundList)
                    {
                        if (ss->minNote <= note
                             && ss->maxNote >= note
                             && ss->audioData.getNumSamples() > 0
                             && ss->sourceSampleRate > 0.0
                             && playingNotes.size() < maximumSimultaneousNotes)
                        {
                            playingNotes.add (new SampledNote (note,
                                                               ss->keyNote,
                                                               0.75f,
                                                               ss->sourceSampleRate,
                                                               sampleRate,
                                                               0,
                                                               ss->audioData,
                                                               ss->fileLengthSamples,
                                                               ss->gainDb,
                                                               ss->pan,
                                                               ss->openEnded));
                        }
                    }
                }
            }

            highlightedNotes = keysDown;
        }
    } // sl released here — lock is free before the log call below

    if (debugLog != nullptr)
        debugLog ("[SfxLockWait] waitMs=%.3f", lockWaitMs);
}

void SamplerPlugin::allNotesOff()
{
    const juce::ScopedLock sl (lock);
    playingNotes.clear();
    highlightedNotes.clear();
}

void SamplerPlugin::applyToBuffer (const PluginRenderContext& fc)
{
    if (fc.destBuffer != nullptr)
    {
        SCOPED_REALTIME_CHECK

        const juce::ScopedLock sl (lock);

        clearChannels (*fc.destBuffer, 2, -1, fc.bufferStartSample, fc.bufferNumSamples);

        if (fc.bufferForMidiMessages != nullptr)
        {
            if (fc.bufferForMidiMessages->isAllNotesOff && ! ignoreTransportAllNotesOff)
            {
                playingNotes.clear();
                highlightedNotes.clear();
            }

            for (auto& m : *fc.bufferForMidiMessages)
            {
                if (m.isNoteOn())
                {
                    const int note = m.getNoteNumber();
                    const int noteTimeSample = juce::roundToInt (m.getTimeStamp() * sampleRate);

                    for (auto playingNote : playingNotes)
                    {
                        if (playingNote->note == note && ! playingNote->openEnded)
                        {
                            playingNote->samplesLeftToPlay = std::min (playingNote->samplesLeftToPlay,
                                                                       std::max (minimumSamplesToPlayWhenStopping,
                                                                                 noteTimeSample));
                            highlightedNotes.clearBit (note);
                        }
                    }

                    for (auto ss : soundList)
                    {
                        if (ss->minNote <= note
                            && ss->maxNote >= note
                            && ss->audioData.getNumSamples() > 0
                            && ss->sourceSampleRate > 0.0
                            && playingNotes.size() < maximumSimultaneousNotes)
                        {
                            highlightedNotes.setBit (note);

                            playingNotes.add (new SampledNote (note,
                                                               ss->keyNote,
                                                               m.getVelocity() / 127.0f,
                                                               ss->sourceSampleRate,
                                                               sampleRate,
                                                               noteTimeSample,
                                                               ss->audioData,
                                                               ss->fileLengthSamples,
                                                               ss->gainDb,
                                                               ss->pan,
                                                               ss->openEnded));
                        }
                    }
                }
                else if (m.isNoteOff())
                {
                    const int note = m.getNoteNumber();
                    const int noteTimeSample = juce::roundToInt (m.getTimeStamp() * sampleRate);

                    for (auto playingNote : playingNotes)
                    {
                        if (playingNote->note == note && ! playingNote->openEnded)
                        {
                            playingNote->samplesLeftToPlay = std::min (playingNote->samplesLeftToPlay,
                                                                       std::max (minimumSamplesToPlayWhenStopping,
                                                                                 noteTimeSample));

                            highlightedNotes.clearBit (note);
                        }
                    }
                }
                else if (m.isAllNotesOff() || m.isAllSoundOff())
                {
                    playingNotes.clear();
                    highlightedNotes.clear();
                }
            }
        }

        for (int i = playingNotes.size(); --i >= 0;)
        {
            auto sn = playingNotes.getUnchecked (i);

            sn->addNextBlock (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples);

            if (sn->isFinished)
                playingNotes.remove (i);
        }
    }
}

//==============================================================================
int SamplerPlugin::getNumSounds() const
{
    return std::accumulate (state.begin(), state.end(), 0,
                            [] (int total, auto v) { return total + (v.hasType (IDs::SOUND) ? 1 : 0); });
}

juce::String SamplerPlugin::getSoundName (int index) const
{
    return getSound (index)[IDs::name];
}

void SamplerPlugin::setSoundName (int index, const juce::String& n)
{
    getSound (index).setProperty (IDs::name, n, getUndoManager());
}

bool SamplerPlugin::hasNameForMidiNoteNumber (int note, int, juce::String& noteName)
{
    juce::String s;

    {
        const juce::ScopedLock sl (lock);

        for (auto ss : soundList)
        {
            if (ss->minNote <= note && ss->maxNote >= note)
            {
                if (s.isNotEmpty())
                    s << " + " << ss->name;
                else
                    s = ss->name;
            }
        }
    }

    noteName = s;
    return true;
}

AudioFile SamplerPlugin::getSoundFile (int index) const
{
    const juce::ScopedLock sl (lock);

    if (auto s = soundList[index])
        return s->audioFile;

    return AudioFile (edit.engine);
}

juce::String SamplerPlugin::getSoundMedia (int index) const
{
    const juce::ScopedLock sl (lock);

    if (auto s = soundList[index])
        return s->source;

    return {};
}

int SamplerPlugin::getKeyNote (int index) const             { return getSound (index)[IDs::keyNote]; }
int SamplerPlugin::getMinKey (int index) const              { return getSound (index)[IDs::minNote]; }
int SamplerPlugin::getMaxKey (int index) const              { return getSound (index)[IDs::maxNote]; }
float SamplerPlugin::getSoundGainDb (int index) const       { return getSound (index)[IDs::gainDb]; }
float SamplerPlugin::getSoundPan (int index) const          { return getSound (index)[IDs::pan]; }
double SamplerPlugin::getSoundStartTime (int index) const   { return getSound (index)[IDs::startTime]; }
bool SamplerPlugin::isSoundOpenEnded (int index) const      { return getSound (index)[IDs::openEnded]; }

double SamplerPlugin::getSoundLength (int index) const
{
    const double l = getSound (index)[IDs::length];

    if (l == 0.0)
    {
        const juce::ScopedLock sl (lock);

        if (auto s = soundList[index])
            return s->length;
    }

    return l;
}

juce::String SamplerPlugin::addSound (const juce::String& source, const juce::String& name,
                                      double startTime, double length, float gainDb)
{
    const int maxNumSamples = 64;

    if (getNumSounds() >= maxNumSamples)
        return TRANS("Can't load any more samples");

    auto v = createValueTree (IDs::SOUND,
                              IDs::source, source,
                              IDs::name, name,
                              IDs::startTime, startTime,
                              IDs::length, length,
                              IDs::keyNote, 72,
                              IDs::minNote, 72 - 24,
                              IDs::maxNote, 72 + 24,
                              IDs::gainDb, gainDb,
                              IDs::pan, (double) 0);

    state.addChild (v, -1, getUndoManager());
    return {};
}

void SamplerPlugin::removeSound (int index)
{
    state.removeChild (index, getUndoManager());

    const juce::ScopedLock sl (lock);
    playingNotes.clear();
    highlightedNotes.clear();
}

void SamplerPlugin::setSoundParams (int index, int keyNote, int minNote, int maxNote)
{
    auto um = getUndoManager();

    auto v = getSound (index);
    v.setProperty (IDs::keyNote, juce::jlimit (0, 127, keyNote), um);
    v.setProperty (IDs::minNote, juce::jlimit (0, 127, std::min (minNote, maxNote)), um);
    v.setProperty (IDs::maxNote, juce::jlimit (0, 127, std::max (minNote, maxNote)), um);
}

void SamplerPlugin::setSoundGains (int index, float gainDb, float pan)
{
    auto um = getUndoManager();

    auto v = getSound (index);
    v.setProperty (IDs::gainDb, juce::jlimit (-48.0f, 48.0f, gainDb), um);
    v.setProperty (IDs::pan,    juce::jlimit (-1.0f,  1.0f,  pan), um);
}

void SamplerPlugin::setSoundExcerpt (int index, double start, double length)
{
    auto um = getUndoManager();

    auto v = getSound (index);
    v.setProperty (IDs::startTime, start, um);
    v.setProperty (IDs::length, length, um);
}

void SamplerPlugin::setSoundOpenEnded (int index, bool b)
{
    auto um = getUndoManager();

    auto v = getSound (index);
    v.setProperty (IDs::openEnded, b, um);
}

void SamplerPlugin::setSoundMedia (int index, const juce::String& source)
{
    auto v = getSound (index);
    v.setProperty (IDs::source, source, getUndoManager());
    triggerAsyncUpdate();
}

juce::ValueTree SamplerPlugin::getSound (int soundIndex) const
{
    int index = 0;

    for (auto v : state)
        if (v.hasType (IDs::SOUND))
            if (index++ == soundIndex)
                return v;

    return {};
}

//==============================================================================
juce::Array<Exportable::ReferencedItem> SamplerPlugin::getReferencedItems()
{
    juce::Array<ReferencedItem> results;

    // must be careful to generate this list in the right order..
    for (int i = 0; i < getNumSounds(); ++i)
    {
        auto v = getSound (i);

        Exportable::ReferencedItem ref;
        ref.itemRef = ProjectItemID::fromProperty (v, IDs::source);
        ref.firstTimeUsed = v[IDs::startTime];
        ref.lengthUsed = v[IDs::length];
        results.add (ref);
    }

    return results;
}

void SamplerPlugin::reassignReferencedItem (const ReferencedItem& item, ProjectItemRef newRef, double newStartTime)
{
    auto index = getReferencedItems().indexOf (item);

    if (index >= 0)
    {
        auto um = getUndoManager();
        auto v = getSound (index);

        if (newRef.isProjectItemID())
        {
            v.setProperty (IDs::source, newRef.toString(), um);
            v.setProperty (IDs::startTime, static_cast<double> (v[IDs::startTime]) - newStartTime, um);
        }
        else
        {
            auto newFile = newRef.resolve (edit.engine);
            v.setProperty (IDs::source, newFile.getFullPathName(), um);
        }
    }
    else
    {
        jassertfalse;
    }
}

void SamplerPlugin::sourceMediaChanged()
{
    const juce::ScopedLock sl (lock);

    for (auto s : soundList)
        s->refreshFile();
}

void SamplerPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    copyValueTree (state, v, getUndoManager());
}

//==============================================================================
SamplerPlugin::SamplerSound::SamplerSound (SamplerPlugin& sf,
                                           const juce::String& source_,
                                           const juce::String& name_,
                                           const double startTime_,
                                           const double length_,
                                           const float gainDb_)
    : owner (sf),
      source (source_),
      name (name_),
      gainDb (juce::jlimit (-48.0f, 48.0f, gainDb_)),
      startTime (startTime_),
      length (length_),
      requestedStartTime (startTime_),
      requestedLength (length_),
      audioFile (owner.edit.engine, SourceFileReference::findFileFromString (owner.edit, source))
{
    setExcerpt (startTime_, length_);

    keyNote = audioFile.getInfo().loopInfo.getRootNote();

    if (keyNote < 0)
        keyNote = 72;

    maxNote = keyNote + 24;
    minNote = keyNote - 24;
}

void SamplerPlugin::SamplerSound::setExcerpt (double startTime_, double length_)
{
    CRASH_TRACER

    sampleDataLoaded = false;

    if (! audioFile.isValid())
    {
        audioFile = AudioFile (owner.edit.engine, SourceFileReference::findFileFromString (owner.edit, source));

       #if JUCE_DEBUG
        if (! audioFile.isValid() && ProjectItemID (source).isValid())
            DBG ("Failed to find media: " << source);
       #endif
    }

    if (audioFile.isValid())
    {
        // Cache the source sample rate so playback never has to query the AudioFile.
        sourceSampleRate = audioFile.getSampleRate();

        const double minLength = 32.0 / sourceSampleRate;

        startTime = juce::jlimit (0.0, audioFile.getLength() - minLength, startTime_);

        if (length_ > 0)
            length = juce::jlimit (minLength, audioFile.getLength() - startTime, length_);
        else
            length = audioFile.getLength();

        fileStartSample   = juce::roundToInt (startTime * sourceSampleRate);
        fileLengthSamples = juce::roundToInt (length * sourceSampleRate);

        if (auto reader = owner.engine.getAudioFileManager().cache.createReader (audioFile))
        {
            audioData.setSize (audioFile.getNumChannels(), fileLengthSamples + 32);
            audioData.clear();

            auto audioDataChannels = ChannelConfiguration::canonical (audioFile.getNumChannels());
            auto channelsToUse = ChannelConfiguration::stereo();

            int total = fileLengthSamples;
            int offset = 0;

            while (total > 0)
            {
                const int numThisTime = std::min (8192, total);
                reader->setReadPosition (fileStartSample + offset);

                if (! reader->readSamples (numThisTime, audioData, audioDataChannels, offset, channelsToUse, 2000))
                {
                    jassertfalse;
                    break;
                }

                offset += numThisTime;
                total -= numThisTime;
            }

            sampleDataLoaded = true;
        }
        else
        {
            audioData.clear();
        }

        // add a quick fade-in if needed..
        int fadeLen = 0;
        for (int i = audioData.getNumChannels(); --i >= 0;)
        {
            const float* d = audioData.getReadPointer (i);

            if (std::abs (*d) > 0.01f)
                fadeLen = 30;
        }

        if (fadeLen > 0)
            AudioFadeCurve::applyCrossfadeSection (audioData, 0, fadeLen, AudioFadeCurve::concave, 0.0f, 1.0f);
    }
    else
    {
        audioFile = AudioFile (owner.edit.engine);
    }
}

void SamplerPlugin::SamplerSound::refreshFile()
{
    audioFile = AudioFile (owner.edit.engine);
    setExcerpt (startTime, length);
}

} // namespace tracktion::inline engine
