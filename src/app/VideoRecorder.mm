/**
 * Copyright (C) 2026 MK124 and contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "VideoRecorder.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "VideoRecorderState.hpp"

namespace m5emulator {

static constexpr int64_t NanosecondsPerSecond = 1'000'000'000;
static constexpr int64_t NanosecondsPerMillisecond = 1'000'000, MillisecondsPerSecond = 1'000;

static constexpr int VideoFrameRate = 60, VideoBitRate = 2'500'000;
static constexpr int AudioBitRate = 96'000;
static constexpr std::size_t AudioBlockFrames = M5RecordingAudioRate / 100;
static constexpr uint64_t AudioFinishTimeoutNs = 5 * NanosecondsPerSecond;
static constexpr int64_t WriterFinishTimeoutNs = 30 * NanosecondsPerSecond;
static constexpr uint64_t AudioDelayNs = NanosecondsPerSecond / 10;
static constexpr uint64_t VideoIntervalNs = NanosecondsPerSecond / VideoFrameRate;

static std::runtime_error recordingError(NSString* message)
{
	return std::runtime_error("Recording: " + std::string(message != nil ? message.UTF8String : "media encoder failed"));
}

static uint64_t audioFramesAt(uint64_t elapsedNs)
{
	return elapsedNs / NanosecondsPerSecond * M5RecordingAudioRate + elapsedNs % NanosecondsPerSecond * M5RecordingAudioRate / NanosecondsPerSecond;
}

VideoRecorder::VideoRecorder() = default;

VideoRecorder::~VideoRecorder()
{
	if (_state) [_state->writer cancelWriting];
}

void VideoRecorder::start(const std::filesystem::path& path, bool audio, const Screen& screen)
{
	@autoreleasepool
	{
		if (_state || finishing()) throw std::runtime_error("Recording is active or still being saved");
		if (std::filesystem::exists(path)) throw std::runtime_error("Recording destination already exists");
		
		auto state = std::make_unique<VideoRecorderState>();
		state->screen = screen;
		NSError* error = nil;
		NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
		state->writer = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&error];
		if (state->writer == nil) throw recordingError(error.localizedDescription);
		
		state->video = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{
			AVVideoCodecKey: AVVideoCodecTypeHEVC,
			AVVideoWidthKey: @(screen.width), AVVideoHeightKey: @(screen.height),
			AVVideoCompressionPropertiesKey: @{
				AVVideoAverageBitRateKey: @(VideoBitRate),
				AVVideoExpectedSourceFrameRateKey: @(VideoFrameRate),
				AVVideoAllowFrameReorderingKey: @NO
			}
		}];
		state->video.expectsMediaDataInRealTime = YES;
		state->pixels = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:state->video sourcePixelBufferAttributes:@{
			(id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
			(id) kCVPixelBufferWidthKey: @(screen.width), (id) kCVPixelBufferHeightKey: @(screen.height),
			(id) kCVPixelBufferIOSurfacePropertiesKey: @{}
		}];
		if (![state->writer canAddInput:state->video]) throw recordingError(@"H.265 encoding is unavailable");
		[state->writer addInput:state->video];
		
		if (audio)
		{
			state->audio = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{
				AVFormatIDKey: @(kAudioFormatMPEG4AAC), AVSampleRateKey: @(M5RecordingAudioRate),
				AVNumberOfChannelsKey: @1, AVEncoderBitRateKey: @(AudioBitRate)
			}];
			state->audio.expectsMediaDataInRealTime = YES;
			if (![state->writer canAddInput:state->audio]) throw recordingError(@"AAC encoding is unavailable");
			[state->writer addInput:state->audio];
		}
		
		if (![state->writer startWriting]) throw recordingError(state->writer.error.localizedDescription);
		[state->writer startSessionAtSourceTime:kCMTimeZero];
		state->startNs = m5RecordingTimeNs();
		
		_path = path;
		_state = std::move(state);
	}
}

void VideoRecorder::appendAudio(const M5RecordingAudio& packet)
{
	if (!_state || _state->audio == nil || packet.kind != M5RecordingPcm) return;
	
	auto& state = *_state;
	const int64_t offsetNs = static_cast<int64_t>(packet.timeNs) - static_cast<int64_t>(state.startNs);
	// Split the time before multiplying; packets may also precede the recording start.
	const int64_t wholeMs = offsetNs / NanosecondsPerMillisecond;
	const int64_t remainderNs = offsetNs % NanosecondsPerMillisecond;
	const int64_t firstFrame = wholeMs * M5RecordingAudioRate / MillisecondsPerSecond + remainderNs * M5RecordingAudioRate / NanosecondsPerSecond;
	
	for (uint32_t i = 0; i < packet.frames; ++i)
	{
		const int64_t frame = firstFrame + i;
		if (frame < 0 || static_cast<uint64_t>(frame) < state.audioFrame) continue;
		if (static_cast<uint64_t>(frame) - state.audioFrame >= state.samples.size()) throw recordingError(@"audio encoder cannot keep up");
		state.samples[frame % state.samples.size()] = packet.samples[i];
	}
}

void VideoRecorder::appendFrame(std::span<const uint16_t> pixels)
{
	if (!_state) return;
	const auto& screen = _state->screen;
	if (pixels.size() != screen.pixelCount()) throw std::runtime_error("Recording frame dimensions do not match");
	@autoreleasepool
	{
		auto& state = *_state;
		if (state.writer.status == AVAssetWriterStatusFailed) throw recordingError(state.writer.error.localizedDescription);
		
		const uint64_t elapsed = m5RecordingTimeNs() - state.startNs;
		if (elapsed > AudioDelayNs) flushAudio(elapsed - AudioDelayNs);
		if ((state.hasVideo && elapsed / VideoIntervalNs <= state.lastVideoNs / VideoIntervalNs) || !state.video.readyForMoreMediaData) return;
		
		CVPixelBufferRef buffer = nullptr;
		if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, state.pixels.pixelBufferPool, &buffer) != kCVReturnSuccess) throw recordingError(@"cannot allocate video frame");
		const CVReturn locked = CVPixelBufferLockBaseAddress(buffer, 0);
		if (locked != kCVReturnSuccess)
		{
			CVPixelBufferRelease(buffer);
			throw recordingError(@"cannot access video frame");
		}
		
		auto* bytes = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
		const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
		for (int y = 0; y < screen.height; ++y)
		{
			auto* row = reinterpret_cast<uint32_t*>(bytes + y * stride);
			for (int x = 0; x < screen.width; ++x)
			{
				const uint16_t pixel = screen.contains(x, y) ? pixels[y * screen.width + x] : 0;
				const unsigned red = pixel >> 11, green = (pixel >> 5) & 63, blue = pixel & 31;
				row[x] = 0xFF000000 | ((red << 3 | red >> 2) << 16) | ((green << 2 | green >> 4) << 8) | (blue << 3 | blue >> 2);
			}
		}
		CVPixelBufferUnlockBaseAddress(buffer, 0);
		
		const uint64_t presentationNs = state.hasVideo ? elapsed : 0;
		const bool appended = [state.pixels appendPixelBuffer:buffer withPresentationTime:CMTimeMake(presentationNs, NanosecondsPerSecond)];
		CVPixelBufferRelease(buffer);
		if (!appended) throw recordingError(state.writer.error.localizedDescription);
		
		state.lastVideoNs = elapsed;
		state.hasVideo = true;
	}
}

void VideoRecorder::finish(uint64_t endNs)
{
	if (!_state) return;
	const uint64_t elapsed = (endNs ? endNs : m5RecordingTimeNs()) - _state->startNs;
	// Transfer exclusive ownership; only the worker touches the encoder after this point.
	_finishing = std::async(std::launch::async, [state = std::move(_state), elapsed] () mutable {
		VideoRecorder recorder;
		recorder._state = std::move(state);
		recorder.finishWriting(elapsed);
	});
}

bool VideoRecorder::pollFinished()
{
	if (!_finishing.valid() || _finishing.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
	_finishing.get();
	return true;
}

void VideoRecorder::finishWriting(uint64_t elapsed)
{
	@autoreleasepool
	{
		try
		{
			[_state->video markAsFinished];
			const uint64_t targetFrame = audioFramesAt(elapsed);
			const uint64_t deadline = m5RecordingTimeNs() + AudioFinishTimeoutNs;
			while (_state->audio != nil && _state->audioFrame < targetFrame)
			{
				flushAudio(elapsed);
				if (_state->writer.status == AVAssetWriterStatusFailed) throw recordingError(_state->writer.error.localizedDescription);
				if (m5RecordingTimeNs() >= deadline) throw recordingError(@"audio encoder did not finish in time");
				if (_state->audioFrame < targetFrame) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			
			[_state->audio markAsFinished];
			[_state->writer endSessionAtSourceTime:CMTimeMake(elapsed, NanosecondsPerSecond)];
			
			dispatch_semaphore_t complete = dispatch_semaphore_create(0);
			[_state->writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(complete); }];
			if (dispatch_semaphore_wait(complete, dispatch_time(DISPATCH_TIME_NOW, WriterFinishTimeoutNs))) throw recordingError(@"encoder did not finish in time");
			if (_state->writer.status != AVAssetWriterStatusCompleted) throw recordingError(_state->writer.error.localizedDescription);
		}
		catch (...)
		{
			[_state->writer cancelWriting];
			_state.reset();
			throw;
		}
		_state.reset();
	}
}

void VideoRecorder::flushAudio(uint64_t throughNs)
{
	auto& state = *_state;
	if (state.audio == nil) return;
	
	const uint64_t throughFrame = audioFramesAt(throughNs);
	while (state.audioFrame < throughFrame && state.audio.readyForMoreMediaData)
	{
		std::array<int16_t, AudioBlockFrames> samples;
		const size_t count = std::min<uint64_t>(samples.size(), throughFrame - state.audioFrame);
		for (size_t i = 0; i < count; ++i) samples[i] = state.samples[(state.audioFrame + i) % state.samples.size()];
		
		AudioStreamBasicDescription format {};
		format.mSampleRate = M5RecordingAudioRate;
		format.mFormatID = kAudioFormatLinearPCM;
		format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
		format.mBytesPerPacket = format.mBytesPerFrame = sizeof(int16_t);
		format.mFramesPerPacket = format.mChannelsPerFrame = 1;
		format.mBitsPerChannel = 16;
		
		CMAudioFormatDescriptionRef description = nullptr;
		CMBlockBufferRef block = nullptr;
		CMSampleBufferRef buffer = nullptr;
		OSStatus status = CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &format, 0, nullptr, 0, nullptr, nullptr, &description);
		if (!status) status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, count * sizeof(int16_t), kCFAllocatorDefault, nullptr, 0, count * sizeof(int16_t), 0, &block);
		if (!status) status = CMBlockBufferReplaceDataBytes(samples.data(), block, 0, count * sizeof(int16_t));
		if (!status) status = CMAudioSampleBufferCreateReadyWithPacketDescriptions(kCFAllocatorDefault, block, description, count, CMTimeMake(state.audioFrame, M5RecordingAudioRate), nullptr, &buffer);
		const bool appended = !status && [state.audio appendSampleBuffer:buffer];
		if (buffer != nullptr) CFRelease(buffer);
		if (block != nullptr) CFRelease(block);
		if (description != nullptr) CFRelease(description);
		if (!appended) throw recordingError(state.writer.error != nil ? state.writer.error.localizedDescription : @"cannot append audio samples");
		
		for (size_t i = 0; i < count; ++i) state.samples[(state.audioFrame + i) % state.samples.size()] = 0;
		state.audioFrame += count;
	}
}

} // namespace m5emulator
