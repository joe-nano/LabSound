
#include "LabSoundConfig.h"

#include "AudioContext.h"

#include "AnalyserNode.h"
#include "AudioBuffer.h"
#include "AudioBufferSourceNode.h"
#include "AudioContextLock.h"
#include "AudioListener.h"
#include "AudioNodeInput.h"
#include "AudioNodeOutput.h"
#include "BiquadFilterNode.h"
#include "ChannelMergerNode.h"
#include "ChannelSplitterNode.h"
#include "ConvolverNode.h"
#include "DefaultAudioDestinationNode.h"
#include "DelayNode.h"
#include "DynamicsCompressorNode.h"
#include "FFTFrame.h"
#include "GainNode.h"
#include "HRTFDatabaseLoader.h"
#include "HRTFPanner.h"
#include "OfflineAudioDestinationNode.h"
#include "OscillatorNode.h"
#include "PannerNode.h"
#include "WaveShaperNode.h"
#include "WaveTable.h"
#include "MediaStream.h"
#include "MediaStreamAudioDestinationNode.h"
#include "MediaStreamAudioSourceNode.h"

#include <wtf/Atomics.h>
#include <stdio.h>

using namespace std;

namespace WebCore
{

// Constructor for realtime rendering
AudioContext::AudioContext()
{
	m_isOfflineContext = false;
	FFTFrame::initialize();
	m_listener = std::make_shared<AudioListener>();
}

// Constructor for offline (non-realtime) rendering.
AudioContext::AudioContext(unsigned numberOfChannels, size_t numberOfFrames, float sampleRate)
{
	m_isOfflineContext = true;

	FFTFrame::initialize();
	m_listener = std::make_shared<AudioListener>();

	// FIXME: the passed in sampleRate MUST match the hardware sample-rate since HRTFDatabaseLoader is a singleton.
	m_hrtfDatabaseLoader = HRTFDatabaseLoader::createAndLoadAsynchronouslyIfNecessary(sampleRate);

	// Create a new destination for offline rendering.
	m_renderTarget = AudioBuffer::create(numberOfChannels, numberOfFrames, sampleRate);

	/*
	// FIXME: offline contexts have limitations on supported sample-rates.
	// Currently all AudioContexts must have the same sample-rate.
	auto loader = HRTFDatabaseLoader::loader();
	if (numberOfChannels > 10 || !isSampleRateRangeGood(sampleRate) || (loader && loader->databaseSampleRate() != sampleRate)) {
		ec = SYNTAX_ERR;
		return 0;
	}
	*/
}

void AudioContext::initHRTFDatabase()
{
	m_hrtfDatabaseLoader = HRTFDatabaseLoader::createAndLoadAsynchronouslyIfNecessary(sampleRate());
}

AudioContext::~AudioContext()
{
#if DEBUG_AUDIONODE_REFERENCES
	fprintf(stderr, "%p: AudioContext::~AudioContext()\n", this);
#endif

	ASSERT(!m_isInitialized);
	ASSERT(m_isStopScheduled);
	ASSERT(!m_nodesToDelete.size());
	ASSERT(!m_referencedNodes.size());
	ASSERT(!m_finishedNodes.size());
	ASSERT(!m_automaticPullNodes.size());
	ASSERT(!m_renderingAutomaticPullNodes.size());
}

void AudioContext::lazyInitialize()
{
	if (!m_isInitialized)
	{
		// Don't allow the context to initialize a second time after it's already been explicitly uninitialized.
		ASSERT(!m_isAudioThreadFinished);
		if (!m_isAudioThreadFinished)
		{
			if (m_destinationNode.get())
			{
				m_destinationNode->initialize();

				if (!isOfflineContext())
				{
					// This starts the audio thread. The destination node's provideInput() method will now be called repeatedly to render audio.
					// Each time provideInput() is called, a portion of the audio stream is rendered. Let's call this time period a "render quantum".
					// NOTE: for now default AudioContext does not need an explicit startRendering() call from JavaScript.
					// We may want to consider requiring it for symmetry with OfflineAudioContext.
					m_destinationNode->startRendering();
				}

			}
			m_isInitialized = true;
		}
	}
}

void AudioContext::clear()
{
	// Audio thread is dead. Nobody will schedule node deletion action. Let's do it ourselves.
	do
	{
		deleteMarkedNodes();
		m_nodesToDelete.insert(m_nodesToDelete.end(), m_nodesMarkedForDeletion.begin(), m_nodesMarkedForDeletion.end());
		m_nodesMarkedForDeletion.clear();
	}
	while (m_nodesToDelete.size());
}

void AudioContext::uninitialize(ContextGraphLock& g)
{
	if (!m_isInitialized)
		return;

	// This stops the audio thread and all audio rendering.
	m_destinationNode->uninitialize();

	// Don't allow the context to initialize a second time after it's already been explicitly uninitialized.
	m_isAudioThreadFinished = true;

	m_referencedNodes.clear();
	m_isInitialized = false;

}

bool AudioContext::isInitialized() const
{
	return m_isInitialized;
}

void AudioContext::incrementConnectionCount()
{
	atomicIncrement(&m_connectionCount); // running tally
}

bool AudioContext::isRunnable() const
{
	if (!isInitialized())
		return false;

	// Check with the HRTF spatialization system to see if it's finished loading.
	return m_hrtfDatabaseLoader->isLoaded();
}

void AudioContext::stop(ContextGraphLock& g)
{
	if (m_isStopScheduled)
		return;

	m_isStopScheduled = true;

	deleteMarkedNodes();

	uninitialize(g);
	clear();
}

std::shared_ptr<MediaStreamAudioSourceNode> AudioContext::createMediaStreamSource(LabSound::ContextGraphLock & g, LabSound::ContextRenderLock & r)
{
	std::shared_ptr<MediaStream> mediaStream = std::make_shared<MediaStream>();

	AudioSourceProvider* provider = 0;

	if (mediaStream->isLocal() && mediaStream->audioTracks()->length())
	{
		provider = destination()->localAudioInputProvider();
	}

	else
	{
		// FIXME: get a provider for non-local MediaStreams (like from a remote peer).
		provider = 0;
	}

	std::shared_ptr<MediaStreamAudioSourceNode> node(new MediaStreamAudioSourceNode(mediaStream, provider, sampleRate()));

	// FIXME: Only stereo streams are supported right now. We should be able to accept multi-channel streams.
	node->setFormat(g, r, 2, sampleRate());

	m_referencedNodes.push_back(node); // context keeps reference until node is disconnected
	return node;
}

void AudioContext::notifyNodeFinishedProcessing(ContextRenderLock& r, AudioNode* node)
{
	ASSERT(r.context());

	for (auto i : m_referencedNodes)
	{
		if (i.get() == node)
		{
			m_finishedNodes.push_back(i);
			return;
		}
	}
	ASSERT(0 == "node to finish not referenced");
}

void AudioContext::derefFinishedSourceNodes(ContextGraphLock& g)
{
	ASSERT(g.context());
	for (unsigned i = 0; i < m_finishedNodes.size(); i++)
		dereferenceSourceNode(g, m_finishedNodes[i]);

	m_finishedNodes.clear();
}

void AudioContext::referenceSourceNode(ContextGraphLock& g, std::shared_ptr<AudioNode> node)
{
	m_referencedNodes.push_back(node);
}

void AudioContext::dereferenceSourceNode(ContextGraphLock& g, std::shared_ptr<AudioNode> node)
{
	ASSERT(g.context());

	for (std::vector<std::shared_ptr<AudioNode>>::iterator i = m_referencedNodes.begin(); i != m_referencedNodes.end(); ++i)
	{
		if (node == *i)
		{
			m_referencedNodes.erase(i);
			break;
		}
	}
}

void AudioContext::holdSourceNodeUntilFinished(std::shared_ptr<AudioScheduledSourceNode> sn)
{
	lock_guard<mutex> lock(automaticSourcesMutex);
	automaticSources.push_back(sn);
}

void AudioContext::handleAutomaticSources()
{
	lock_guard<mutex> lock(automaticSourcesMutex);
	for (auto i = automaticSources.begin(); i != automaticSources.end(); ++i)
	{
		if ((*i)->hasFinished())
		{
			i = automaticSources.erase(i);
			if (i == automaticSources.end())
				break;
		}
	}
}

void AudioContext::handlePreRenderTasks(ContextRenderLock& r)
{
	ASSERT(r.context());

	// At the beginning of every render quantum, try to update the internal rendering graph state (from main thread changes).
	AudioSummingJunction::handleDirtyAudioSummingJunctions(r);
	updateAutomaticPullNodes();
}

void AudioContext::handlePostRenderTasks(ContextRenderLock& r)
{
	ASSERT(r.context());

	// Don't delete in the real-time thread. Let the main thread do it because the clean up may take time
	scheduleNodeDeletion(r);

	AudioSummingJunction::handleDirtyAudioSummingJunctions(r);
	updateAutomaticPullNodes();

	handleAutomaticSources();
}

void AudioContext::connect(std::shared_ptr<AudioNode> from, std::shared_ptr<AudioNode> to)
{
	lock_guard<mutex> lock(automaticSourcesMutex);
	pendingNodeConnections.emplace_back(from, to, true);
}

void AudioContext::connect(std::shared_ptr<AudioNodeInput> fromInput, std::shared_ptr<AudioNodeOutput> toOutput)
{
	lock_guard<mutex> lock(automaticSourcesMutex);
	pendingConnections.emplace_back(PendingConnection<AudioNodeInput, AudioNodeOutput>(fromInput, toOutput, true));
}

void AudioContext::disconnect(std::shared_ptr<AudioNode> from, std::shared_ptr<AudioNode> to)
{
	lock_guard<mutex> lock(automaticSourcesMutex);
	pendingNodeConnections.emplace_back(from, to, false);
}

void AudioContext::disconnect(std::shared_ptr<AudioNode> from)
{
	lock_guard<mutex> lock(automaticSourcesMutex);
	pendingNodeConnections.emplace_back(from, std::shared_ptr<AudioNode>(), false);
}

void AudioContext::disconnect(std::shared_ptr<AudioNodeOutput> toOutput)
{
	lock_guard<mutex> lock(automaticSourcesMutex);
	pendingConnections.emplace_back(PendingConnection<AudioNodeInput, AudioNodeOutput>(std::shared_ptr<AudioNodeInput>(), toOutput, false));
}

void AudioContext::update(ContextGraphLock& g)
{
	{
		lock_guard<mutex> lock(automaticSourcesMutex);
		for (auto i : pendingConnections)
		{
			if (i.connect)
			{
				AudioNodeInput::connect(g, i.from, i.to);
			}
			else
			{
				AudioNodeOutput::disconnectAll(g, i.to);
			}
		}
		pendingConnections.clear();

		for (auto i : pendingNodeConnections)
		{
			if (i.connect)
			{
				AudioNodeInput::connect(g, i.to->input(0), i.from->output(0));
				referenceSourceNode(g, i.from);
				referenceSourceNode(g, i.to);
				atomicIncrement(&i.from->m_connectionRefCount);
				atomicIncrement(&i.to->m_connectionRefCount);
				i.from->enableOutputsIfNecessary(g);
				i.to->enableOutputsIfNecessary(g);
			}
			else
			{
				if (!i.from)
				{
					ExceptionCode ec = NO_ERR;
					atomicDecrement(&i.to->m_connectionRefCount);
					i.to->disconnect(g.context(), 0, ec);
					i.to->disableOutputsIfNecessary(g);
				}
				else
				{
					atomicDecrement(&i.from->m_connectionRefCount);
					atomicDecrement(&i.to->m_connectionRefCount);
					AudioNodeInput::disconnect(g, i.from->input(0), i.to->output(0));
					dereferenceSourceNode(g, i.from);
					dereferenceSourceNode(g, i.to);
					i.from->disableOutputsIfNecessary(g);
					i.to->disableOutputsIfNecessary(g);
				}
			}
		}
		pendingNodeConnections.clear();
	}

	// Dynamically clean up nodes which are no longer needed.
	derefFinishedSourceNodes(g);
}

void AudioContext::markForDeletion(ContextRenderLock& r, AudioNode* node)
{
	ASSERT(r.context());
	for (auto i : m_referencedNodes)
	{
		if (i.get() == node)
		{
			m_nodesMarkedForDeletion.push_back(i);
			return;
		}
	}

	ASSERT(0 == "Attempting to delete unreferenced node");
}

void AudioContext::scheduleNodeDeletion(ContextRenderLock& r)
{
	// &&& all this deletion stuff should be handled by a concurrent queue - simply have only a m_nodesToDelete concurrent queue and ditch the marked vector
	// then this routine sould go away completely
	// node->deref is the only caller, it should simply add itself to the scheduled deletion queue
	// marked for deletion should go away too

	bool isGood = m_isInitialized && r.context();
	ASSERT(isGood);
	if (!isGood)
		return;

	if (m_nodesMarkedForDeletion.size() && !m_isDeletionScheduled)
	{
		m_nodesToDelete.insert(m_nodesToDelete.end(), m_nodesMarkedForDeletion.begin(), m_nodesMarkedForDeletion.end());
		m_nodesMarkedForDeletion.clear();

		m_isDeletionScheduled = true;

		deleteMarkedNodes();
	}
}

void AudioContext::deleteMarkedNodes()
{
	// Fixme: thread safety
	m_nodesToDelete.clear();
	m_isDeletionScheduled = false;
}


void AudioContext::addAutomaticPullNode(std::shared_ptr<AudioNode> node)
{
	lock_guard<mutex> lock(automaticSourcesMutex);
	if (m_automaticPullNodes.find(node) == m_automaticPullNodes.end())
	{
		m_automaticPullNodes.insert(node);
		m_automaticPullNodesNeedUpdating = true;
	}
}

void AudioContext::removeAutomaticPullNode(std::shared_ptr<AudioNode> node)
{
	lock_guard<mutex> lock(automaticSourcesMutex);
	auto it = m_automaticPullNodes.find(node);
	if (it != m_automaticPullNodes.end())
	{
		m_automaticPullNodes.erase(it);
		m_automaticPullNodesNeedUpdating = true;
	}
}

void AudioContext::updateAutomaticPullNodes()
{
	if (m_automaticPullNodesNeedUpdating)
	{
		lock_guard<mutex> lock(automaticSourcesMutex);

		// Copy from m_automaticPullNodes to m_renderingAutomaticPullNodes.
		m_renderingAutomaticPullNodes.resize(m_automaticPullNodes.size());

		unsigned j = 0;
		for (auto i = m_automaticPullNodes.begin(); i != m_automaticPullNodes.end(); ++i, ++j)
		{
			m_renderingAutomaticPullNodes[j] = *i;
		}

		m_automaticPullNodesNeedUpdating = false;
	}
}

void AudioContext::processAutomaticPullNodes(ContextRenderLock& r, size_t framesToProcess)
{
	for (unsigned i = 0; i < m_renderingAutomaticPullNodes.size(); ++i)
		m_renderingAutomaticPullNodes[i]->processIfNecessary(r, framesToProcess);
}

void AudioContext::setDestinationNode(std::shared_ptr<AudioDestinationNode> node) 
{ 
	m_destinationNode = node; 
}

std::shared_ptr<AudioDestinationNode> AudioContext::destination() 
{ 
	return m_destinationNode; 
}

bool AudioContext::isOfflineContext() 
{ 
	return m_isOfflineContext; 
}

size_t AudioContext::currentSampleFrame() const 
{ 
	return m_destinationNode->currentSampleFrame(); 
}

double AudioContext::currentTime() const 
{  
	return m_destinationNode->currentTime();
}

float AudioContext::sampleRate() const 
{  
	return m_destinationNode ? m_destinationNode->sampleRate() : AudioDestination::hardwareSampleRate(); 
}

AudioListener * AudioContext::listener() 
{ 
	return m_listener.get(); 
}

unsigned long AudioContext::activeSourceCount() const 
{ 
	return static_cast<unsigned long>(m_activeSourceCount); 
}

void AudioContext::startRendering()
{
	destination()->startRendering();
}

void AudioContext::incrementActiveSourceCount()
{
	atomicIncrement(&m_activeSourceCount);
}

void AudioContext::decrementActiveSourceCount()
{
	atomicDecrement(&m_activeSourceCount);
}

} // End namespace WebCore
