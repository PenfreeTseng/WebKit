/*
 * Copyright (C) 2020 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "FormatReader.h"

#if HAVE(MT_PLUGIN_FORMAT_READER)

#include "TrackReader.h"
#include <WebCore/AudioTrackPrivate.h>
#include <WebCore/ContentType.h>
#include <WebCore/InbandTextTrackPrivate.h>
#include <WebCore/MediaSample.h>
#include <WebCore/SourceBufferParser.h>
#include <WebCore/VideoTrackPrivate.h>
#include <pal/avfoundation/MediaTimeAVFoundation.h>
#include <wtf/WorkQueue.h>

#include <pal/cocoa/MediaToolboxSoftLink.h>

WTF_DECLARE_CF_TYPE_TRAIT(MTPluginFormatReader);

namespace WebKit {

using namespace PAL;
using namespace WebCore;

CMBaseClassID FormatReader::wrapperClassID()
{
    return MTPluginFormatReaderGetClassID();
}

CoreMediaWrapped<FormatReader>* FormatReader::unwrap(CMBaseObjectRef object)
{
    return unwrap(checked_cf_cast<WrapperRef>(object));
}

RefPtr<FormatReader> FormatReader::create(Allocator&& allocator)
{
    return adoptRef(new (allocator) FormatReader(WTFMove(allocator)));
}

FormatReader::FormatReader(Allocator&& allocator)
    : CoreMediaWrapped(WTFMove(allocator))
    , m_duration(MediaTime::invalidTime())
{
}

void FormatReader::startOnMainThread(MTPluginByteSourceRef byteSource)
{
    ASSERT(!isMainThread());
    callOnMainThread([this, protectedThis = makeRef(*this), byteSource = retainPtr(byteSource)]() mutable {
        parseByteSource(WTFMove(byteSource));
    });
}

static WorkQueue& readerQueue()
{
    static auto& queue = WorkQueue::create("WebKit FormatReader Queue", WorkQueue::Type::Concurrent).leakRef();
    return queue;
}

void FormatReader::parseByteSource(RetainPtr<MTPluginByteSourceRef>&& byteSource)
{
    ASSERT(isMainThread());

    static NeverDestroyed<ContentType> contentType("video/webm"_s);
    auto parser = SourceBufferParser::create(contentType);
    if (!parser) {
        m_parseTracksStatus = kMTPluginFormatReaderError_AllocationFailure;
        return;
    }

    // Set a minimum audio sample duration of 0 so the parser creates indivisible samples with byte source ranges.
    parser->setMinimumAudioSampleDuration(0);

    auto locker = holdLock(m_parseTracksLock);

    m_byteSource = WTFMove(byteSource);
    m_parseTracksStatus = WTF::nullopt;
    m_duration = MediaTime::invalidTime();
    m_trackReaders.clear();

    parser->setDidParseInitializationDataCallback([this, protectedThis = makeRef(*this)](SourceBufferParser::InitializationSegment&& initializationSegment, CompletionHandler<void()>&& completionHandler) {
        didParseTracks(WTFMove(initializationSegment), noErr);
        completionHandler();
    });

    parser->setDidEncounterErrorDuringParsingCallback([this, protectedThis = makeRef(*this)](uint64_t errorCode) {
        didParseTracks({ }, errorCode);
    });

    parser->setDidProvideMediaDataCallback([this, protectedThis = makeRef(*this)](Ref<MediaSample>&& mediaSample, uint64_t trackID, const String& mediaType) {
        didProvideMediaData(WTFMove(mediaSample), trackID, mediaType);
    });

    readerQueue().dispatch([this, protectedThis = makeRef(*this), byteSource = m_byteSource, parser = parser.releaseNonNull()]() mutable {
        parser->appendData(WTFMove(byteSource));
        callOnMainThread([this, protectedThis = makeRef(*this), parser = WTFMove(parser)]() mutable {
            finishParsing(WTFMove(parser));
        });
    });
}

void FormatReader::didParseTracks(SourceBufferPrivateClient::InitializationSegment&& segment, uint64_t errorCode)
{
    ASSERT(isMainThread());

    auto locker = holdLock(m_parseTracksLock);
    ASSERT(!m_parseTracksStatus);
    ASSERT(m_duration.isInvalid());
    ASSERT(m_trackReaders.isEmpty());

    m_parseTracksStatus = errorCode ? kMTPluginFormatReaderError_ParsingFailure : noErr;
    m_duration = WTFMove(segment.duration);

    for (auto& videoTrack : segment.videoTracks) {
        if (auto trackReader = TrackReader::create(allocator(), *this, videoTrack.track.get()))
            m_trackReaders.append(trackReader.releaseNonNull());
        // FIXME: How do we know which tracks should be enabled?
        if (m_trackReaders.size() == 1)
            m_trackReaders[0]->setEnabled(true);
    }

    for (auto& audioTrack : segment.audioTracks) {
        if (auto trackReader = TrackReader::create(allocator(), *this, audioTrack.track.get()))
            m_trackReaders.append(trackReader.releaseNonNull());
        // FIXME: How do we know which tracks should be enabled?
        if (m_trackReaders.size() == segment.videoTracks.size() + 1)
            m_trackReaders[segment.videoTracks.size()]->setEnabled(true);
    }

    for (auto& textTrack : segment.textTracks) {
        if (auto trackReader = TrackReader::create(allocator(), *this, textTrack.track.get()))
            m_trackReaders.append(trackReader.releaseNonNull());
    }
    
    m_parseTracksCondition.notifyAll();
}

void FormatReader::didProvideMediaData(Ref<MediaSample>&& mediaSample, uint64_t trackID, const String&)
{
    ASSERT(isMainThread());

    auto locker = holdLock(m_parseTracksLock);
    auto trackIndex = m_trackReaders.findMatching([&](auto& track) {
        return track->trackID() == trackID;
    });

    if (trackIndex != notFound)
        m_trackReaders[trackIndex]->addSample(WTFMove(mediaSample), m_byteSource.get());
}

void FormatReader::finishParsing(Ref<SourceBufferParser>&& parser)
{
    ASSERT(isMainThread());

    auto locker = holdLock(m_parseTracksLock);
    ASSERT(m_parseTracksStatus.hasValue());

    for (auto& trackReader : m_trackReaders)
        trackReader->finishParsing();

    parser->setDidParseInitializationDataCallback(nullptr);
    parser->setDidEncounterErrorDuringParsingCallback(nullptr);
    parser->setDidProvideMediaDataCallback(nullptr);
    parser->resetParserState();
}

OSStatus FormatReader::copyProperty(CFStringRef key, CFAllocatorRef allocator, void* valueCopy)
{
    auto locker = holdLock(m_parseTracksLock);
    m_parseTracksCondition.wait(m_parseTracksLock, [&] {
        return m_parseTracksStatus.hasValue();
    });

    if (CFEqual(key, PAL::get_MediaToolbox_kMTPluginFormatReaderProperty_Duration())) {
        if (auto leakedDuration = adoptCF(CMTimeCopyAsDictionary(PAL::toCMTime(m_duration), allocator)).leakRef()) {
            *reinterpret_cast<CFDictionaryRef*>(valueCopy) = leakedDuration;
            return noErr;
        }
    }

    return kCMBaseObjectError_ValueNotAvailable;
}

OSStatus FormatReader::copyTrackArray(CFArrayRef* trackArrayCopy)
{
    ASSERT(!isMainThread());

    auto locker = holdLock(m_parseTracksLock);
    m_parseTracksCondition.wait(m_parseTracksLock, [&] {
        return m_parseTracksStatus.hasValue();
    });

    if (*m_parseTracksStatus != noErr)
        return *m_parseTracksStatus;

    auto mutableArray = adoptCF(CFArrayCreateMutable(allocator(), Checked<CFIndex>(m_trackReaders.size()).unsafeGet(), &kCFTypeArrayCallBacks));
    for (auto& trackReader : m_trackReaders)
        CFArrayAppendValue(mutableArray.get(), trackReader->wrapper());

    *trackArrayCopy = adoptCF(CFArrayCreateCopy(allocator(), mutableArray.get())).leakRef();
    return noErr;
}

} // namespace WebKit

#endif // HAVE(MT_PLUGIN_FORMAT_READER)
