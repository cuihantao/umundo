/**
 *  @file
 *  @author     2012 Stefan Radomski (stefan.radomski@cs.tu-darmstadt.de)
 *  @copyright  Simplified BSD
 *
 *  @cond
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the FreeBSD license as published by the FreeBSD
 *  project.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 *  You should have received a copy of the FreeBSD license along with this
 *  program. If not, see <http://www.opensource.org/licenses/bsd-license>.
 *  @endcond
 */

#ifdef WIN32
#include <time.h>
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#endif

#include "umundo/connection/zeromq/ZeroMQPublisher.h"
#include "umundo/connection/zeromq/ZeroMQNode.h"
#include "umundo/Message.h"
#include "umundo/UUID.h"

#include "umundo/config.h"
#if defined UNIX || defined IOS || defined IOSSIM
#include <string.h> // strlen, memcpy
#include <stdio.h> // snprintf

#endif


namespace umundo {

ZeroMQPublisher::ZeroMQPublisher() : _comressionLevel(-1), _compressionWithState(false), _compressionContext(NULL) {
    _refreshedCompressionContext = 0;
    _compressionRefreshInterval = 0;
}

void ZeroMQPublisher::init(const Options* config) {
	RScopeLock lock(_mutex);

	_transport = "tcp";

	(_pubSocket = zmq_socket(ZeroMQNode::getZeroMQContext(), ZMQ_PUB)) || UM_LOG_WARN("zmq_socket: %s",zmq_strerror(errno));

	int hwm = NET_ZEROMQ_SND_HWM;
	std::string pubId("um.pub.intern." + _uuid);

//	zmq_setsockopt(_pubSocket, ZMQ_IDENTITY, pubId.c_str(), pubId.length()) && UM_LOG_WARN("zmq_setsockopt: %s",zmq_strerror(errno));
	zmq_setsockopt(_pubSocket, ZMQ_SNDHWM, &hwm, sizeof(hwm)) && UM_LOG_WARN("zmq_setsockopt: %s",zmq_strerror(errno));
	zmq_bind(_pubSocket, std::string("inproc://" + pubId).c_str());

	std::map<std::string, std::string> options = config->getKVPs();

	if (options.find("pub.compression.type") != options.end()) {
		_compressionType = options["pub.compression.type"];
	}
	if (options.find("pub.compression.level") != options.end()) {
		_comressionLevel = strTo<int>(options["pub.compression.level"]);
	}
    if (options.find("pub.compression.withState") != options.end()) {
        _compressionWithState = strTo<bool>(options["pub.compression.withState"]);
    }
    if (options.find("pub.compression.refreshInterval") != options.end()) {
        int refreshInterval = strTo<int>(options["pub.compression.refreshInterval"]);
        _compressionRefreshInterval = refreshInterval;
    }
    
	UM_LOG_INFO("creating internal publisher%s for %s on %s", (_compressionType.size() > 0 ? " with compression" : ""), _channelName.c_str(), std::string("inproc://" + pubId).c_str());

}

ZeroMQPublisher::~ZeroMQPublisher() {
	UM_LOG_INFO("deleting publisher for %s", _channelName.c_str());

	std::string pubId("um.pub.intern." + _uuid);
//	zmq_unbind(_pubSocket, std::string("inproc://" + pubId).c_str()) && UM_LOG_WARN("zmq_unbind: %s", zmq_strerror(errno));
	zmq_close(_pubSocket);

	// clean up pending messages
	std::map<std::string, std::list<std::pair<uint64_t, Message*> > >::iterator queuedMsgSubIter = _queuedMessages.begin();
	while(queuedMsgSubIter != _queuedMessages.end()) {
		std::list<std::pair<uint64_t, Message*> >::iterator queuedMsgIter = queuedMsgSubIter->second.begin();
		while(queuedMsgIter != queuedMsgSubIter->second.end()) {
			delete (queuedMsgIter->second);
			queuedMsgIter++;
		}
		queuedMsgSubIter++;
	}

}

SharedPtr<Implementation> ZeroMQPublisher::create() {
	return SharedPtr<ZeroMQPublisher>(new ZeroMQPublisher());
}

void ZeroMQPublisher::suspend() {
	RScopeLock lock(_mutex);
	if (_isSuspended)
		return;
	_isSuspended = true;
};

void ZeroMQPublisher::resume() {
	RScopeLock lock(_mutex);
	if (!_isSuspended)
		return;
	_isSuspended = false;
};

int ZeroMQPublisher::waitForSubscribers(int count, int timeoutMs) {
	RScopeLock lock(_mutex);
	uint64_t now = Thread::getTimeStampMs();
	while (unique_keys(_domainSubs) < (unsigned int)count) {
		_pubLock.wait(_mutex, timeoutMs);
		if (timeoutMs > 0 && Thread::getTimeStampMs() - timeoutMs > now)
			break;
	}
	/**
	 * TODO: we get notified when the subscribers uuid occurs, that
	 * might be before the actual channel is subscribed. I posted
	 * to the ZeroMQ ML regarding socket IDs with every subscription
	 * message. Until then, sleep a bit.
	 * Update: I use a late alphabetical order to get the subscriber id last
	 */
//  Thread::sleepMs(20);
	return unique_keys(_domainSubs);
}

void ZeroMQPublisher::added(const SubscriberStub& sub, const NodeStub& node) {
	RScopeLock lock(_mutex);

	_subs[sub.getUUID()] = sub;

	// do we already now about this sub via this node?
	std::pair<_domainSubs_t::iterator, _domainSubs_t::iterator> subIter = _domainSubs.equal_range(sub.getUUID());
	while(subIter.first != subIter.second) {
		if (subIter.first->second.first.getUUID() == node.getUUID())
			return; // we already know about this sub from this node
		subIter.first++;
	}

	_domainSubs.insert(std::make_pair(sub.getUUID(), std::make_pair(node, sub)));

	UM_LOG_INFO("Publisher %s on channel %s received subscriber %s at node %s",
	            SHORT_UUID(_uuid).c_str(), _channelName.c_str(), SHORT_UUID(sub.getUUID()).c_str(), SHORT_UUID(node.getUUID()).c_str());

	if (_greeter != NULL && _domainSubs.count(sub.getUUID()) == 1) {
		// only perform greeting for first occurence of subscriber
		Publisher pub(StaticPtrCast<PublisherImpl>(shared_from_this()));
		_greeter->welcome(pub, sub);
	}

    // reset compression context
    _compressionContext = NULL;
    
	if (_queuedMessages.find(sub.getUUID()) != _queuedMessages.end()) {
		UM_LOG_INFO("Subscriber with queued messages joined, sending %d old messages", _queuedMessages[sub.getUUID()].size());
		std::list<std::pair<uint64_t, Message*> >::iterator msgIter = _queuedMessages[sub.getUUID()].begin();
		while(msgIter != _queuedMessages[sub.getUUID()].end()) {
			send(msgIter->second);
			msgIter++;
		}
		_queuedMessages.erase(sub.getUUID());
	}
	UMUNDO_SIGNAL(_pubLock);
}

void ZeroMQPublisher::removed(const SubscriberStub& sub, const NodeStub& node) {
	RScopeLock lock(_mutex);

	// do we now about this sub via this node?
	bool subscriptionFound = false;
	std::pair<_domainSubs_t::iterator, _domainSubs_t::iterator> subIter = _domainSubs.equal_range(sub.getUUID());
	while(subIter.first != subIter.second) {
		if (subIter.first->second.first.getUUID() == node.getUUID()) {
			subscriptionFound = true;
			break;
		}
		subIter.first++;
	}
	if (!subscriptionFound)
		return;

	UM_LOG_INFO("Publisher %s lost subscriber %s on node %s for channel %s", SHORT_UUID(_uuid).c_str(), SHORT_UUID(sub.getUUID()).c_str(), SHORT_UUID(node.getUUID()).c_str(), _channelName.c_str());

	if (_domainSubs.count(sub.getUUID()) == 1) { // about to vanish
		if (_greeter != NULL) {
			Publisher pub(Publisher(StaticPtrCast<PublisherImpl>(shared_from_this())));
			_greeter->farewell(pub, sub);
		}
		_subs.erase(sub.getUUID());
	}

	_domainSubs.erase(subIter.first);
	UMUNDO_SIGNAL(_pubLock);
}

void ZeroMQPublisher::send(Message* msg) {
	if (_isSuspended) {
		UM_LOG_WARN("Not sending message on suspended publisher");
		return;
	}

	// topic name or explicit subscriber id is first message in envelope
	zmq_msg_t channelEnvlp;

	if (msg->getMeta().find("um.sub") != msg->getMeta().end()) {
		// explicit destination
		if (_domainSubs.count(msg->getMeta("um.sub")) == 0 && !msg->isQueued()) {
			UM_LOG_INFO("Subscriber %s is not (yet) connected on %s - queuing message", msg->getMeta("um.sub").c_str(), _channelName.c_str());
			Message* queuedMsg = new Message(*msg); // copy message
			queuedMsg->setQueued(true);
			_queuedMessages[msg->getMeta("um.sub")].push_back(std::make_pair(Thread::getTimeStampMs(), queuedMsg));
			return;
		}
		ZMQ_PREPARE_STRING(channelEnvlp, std::string("~" + msg->getMeta("um.sub")).c_str(), msg->getMeta("um.sub").size() + 1);
	} else {
		// everyone on channel
		ZMQ_PREPARE_STRING(channelEnvlp, _channelName.c_str(), _channelName.size());
	}
	zmq_sendmsg(_pubSocket, &channelEnvlp, ZMQ_SNDMORE) >= 0 || UM_LOG_WARN("zmq_sendmsg: %s", zmq_strerror(errno));
	zmq_msg_close(&channelEnvlp) && UM_LOG_WARN("zmq_msg_close: %s",zmq_strerror(errno));

	// user supplied mandatory meta fields
    for (std::map<std::string, std::string>::const_iterator metaIter = _mandatoryMeta.begin(); metaIter != _mandatoryMeta.end(); metaIter++) {
        msg->putMeta(metaIter->first, metaIter->second);
    }

    // default meta fields
    msg->putMeta("um.pub", _uuid);
    msg->putMeta("um.proc", procUUID);
    msg->putMeta("um.host", hostUUID);

    
    /**
                                Bits
     Message Version            8,
     Publisher UUID             128,
     Compressed Header          1,
     Compression KeyFrame       1,
     Compression Type           3,
     Reserved                   3,
     Header Length < 254        8  (Len < 254),
     Header Length < (1 << 16)  16 (Len == 254),
     Header Length < (1 << 64)  48 (Len == 255),
     Header Data                ..
     Payload Data               ..
     
     */
    RScopeLock lock(_mutex);

#define MAX_MESSAGE_PRELUDE \
    1 +  /* Message version */ \
    16 + /* Pub UUID */ \
    1 +  /* Header Flags */ \
    9    /* Header Length (may be smaller by preludeOffset) */
    
    bool isCompressionKeyFrame = !_compressionWithState;
    if (_compressionWithState) {
        // do we need to reset the compression context?
        if (_compressionRefreshInterval > 0) {
            uint64_t now = Thread::getTimeStampMs();

            if (now - _compressionRefreshInterval > _refreshedCompressionContext) {
                _compressionContext = NULL;
                _refreshedCompressionContext = now;
                // UM_LOG_WARN("asdf: %d:%d > %d:%d", elapsed.tv_sec, elapsed.tv_usec, _compressionRefreshInterval.tv_sec, _compressionRefreshInterval.tv_usec);
            }
        }
        
        if (_compressionContext == NULL && _compressionType.size() > 0) {
            _compressionContext = Message::createCompression();
            isCompressionKeyFrame = true;
//            UM_LOG_WARN("New compression context!");
        }
    } else {
        _compressionContext = NULL;
    }
    
    
    if (isCompressionKeyFrame)
        UM_LOG_INFO("Publisher %s on channel %s sending compression keyframe",
                    SHORT_UUID(_uuid).c_str(), _channelName.c_str());

    // we can only know the size of the header once we compressed it
    size_t preludeOffset = 0;
    size_t headerSize = 0;
    size_t payloadSize = 0;
    
    // maximum size for compressed header and payload
    if (_compressionType.size() > 0) {
        // compress
        headerSize  = msg->getCompressBounds(_compressionType, _compressionContext, Message::HEADER);
        payloadSize = msg->getCompressBounds(_compressionType, _compressionContext, Message::PAYLOAD);
    } else {
        // do not compress
        headerSize = msg->getHeaderDataSize();
        payloadSize = msg->size();
    }
    // this buffer has to be large enough to hold the complete message
    char* onwire = (char*)malloc(headerSize + payloadSize + MAX_MESSAGE_PRELUDE);
    
    if (_compressionType.size() > 0) {
        headerSize  = msg->compress(_compressionType, _compressionContext, onwire + MAX_MESSAGE_PRELUDE, headerSize, Message::HEADER);
        payloadSize = msg->compress(_compressionType, _compressionContext, onwire + headerSize + MAX_MESSAGE_PRELUDE, payloadSize, Message::PAYLOAD);
    } else {
        msg->writeHeaders(onwire + MAX_MESSAGE_PRELUDE, headerSize);
        memcpy(onwire + headerSize + MAX_MESSAGE_PRELUDE, msg->data(), payloadSize);
    }
    
    // we may need to trim some bytes in front as the header length is dynamic
    if (headerSize < 254) {
        preludeOffset = 8; // only a single byte for the header
    } else if (headerSize < (1 << 16)) {
        preludeOffset = 6; // three bytes for the header
    }
    
    // advance buffer write pointer to account for dynamic header size
    char* onwireStart = onwire + preludeOffset;
    char* writePtr = onwireStart;
    
    // first byte is the message version
    writePtr = Message::write(writePtr, (uint8_t)Message::UM_MSG_VERSION);
    assert(writePtr = onwireStart + 1);
    
    // then 16 bytes publisher uuid
    writePtr = UUID::writeHexToBin(writePtr, _uuid);
    assert(writePtr = onwireStart + 1 + 16);

    // second byte is the compression info
    writePtr[0] = '0';
    if (_compressionType.size() > 0) {
        writePtr[0] |= Message::UM_COMPR_MSG;
        if (isCompressionKeyFrame) {
            writePtr[0] |= Message::UM_COMPR_KEYFRAME;
        }
        
        if (_compressionType == "lz4") {
            writePtr[0] |= Message::UM_COMPR_LZ4;
        }
    }
    writePtr++;
    
    assert(writePtr = onwireStart + 1 + 16 + 1);

    writePtr = Message::writeCompact(writePtr, headerSize, (headerSize + payloadSize + MAX_MESSAGE_PRELUDE) - preludeOffset);
    assert(writePtr = onwireStart + 1 + 16 + 1 + (9 - preludeOffset));
    assert(writePtr == onwire + MAX_MESSAGE_PRELUDE);
    
    size_t msgSize = (headerSize + payloadSize + (MAX_MESSAGE_PRELUDE - preludeOffset));
    
    assert(onwireStart[0] == Message::UM_MSG_VERSION);
    
    zmq_msg_t zqmMsg;
    ZMQ_PREPARE_DATA(zqmMsg, onwireStart, msgSize); // this is yet another memcpy :(

    free(onwire);
    
    zmq_sendmsg(_pubSocket, &zqmMsg, 0) >= 0 || UM_LOG_WARN("zmq_sendmsg: %s", zmq_strerror(errno));
    zmq_msg_close(&zqmMsg) && UM_LOG_WARN("zmq_msg_close: %s", zmq_strerror(errno));

#if 0
	// all our meta information
	for (std::map<std::string, std::string>::iterator metaIter = metaKeys.begin(); metaIter != metaKeys.end(); metaIter++) {

		// string length of key + value + two null bytes as string delimiters
		size_t metaSize = (metaIter->first).length() + (metaIter->second).length() + 2;
		zmq_msg_t metaMsg;
		ZMQ_PREPARE(metaMsg, metaSize);

		char* writePtr = (char*)zmq_msg_data(&metaMsg);
		memcpy(writePtr, (metaIter->first).data(), (metaIter->first).length());

		// indexes start at zero, so length is the byte after the string
		((char*)zmq_msg_data(&metaMsg))[(metaIter->first).length()] = '\0';
		assert(strlen((char*)zmq_msg_data(&metaMsg)) == (metaIter->first).length());
		assert(strlen(writePtr) == (metaIter->first).length()); // just to be sure

		// increment write pointer
		writePtr += (metaIter->first).length() + 1;

		memcpy(writePtr,
		       (metaIter->second).data(),
		       (metaIter->second).length());

		// first string + null byte + second string
		((char*)zmq_msg_data(&metaMsg))[(metaIter->first).length() + 1 + (metaIter->second).length()] = '\0';
		assert(strlen(writePtr) == (metaIter->second).length());

		zmq_sendmsg(_pubSocket, &metaMsg, ZMQ_SNDMORE) >= 0 || UM_LOG_WARN("zmq_sendmsg: %s", zmq_strerror(errno));
		zmq_msg_close(&metaMsg) && UM_LOG_WARN("zmq_msg_close: %s",zmq_strerror(errno));
	}

	// data as the second part of a multipart message
	zmq_msg_t payload;
	zmq_msg_init(&payload) && UM_LOG_WARN("zmq_msg_init: %s", zmq_strerror(errno));
	zmq_msg_init_size (&payload, msg->size()) && UM_LOG_WARN("zmq_msg_init_size: %s", zmq_strerror(errno));
	if (msg->_doneCallback) {
		zmq_msg_init_data(&payload, (void*)msg->data(), msg->size(), msg->_doneCallback, msg->_hint);
	} else {
		memcpy(zmq_msg_data(&payload), msg->data(), msg->size());
	}
    
    zmq_sendmsg(_pubSocket, &payload, 0) >= 0 || UM_LOG_WARN("zmq_sendmsg: %s", zmq_strerror(errno));
    zmq_msg_close(&payload) && UM_LOG_WARN("zmq_msg_close: %s", zmq_strerror(errno));

#endif

#if 0
	/**
	 * avoid message envelopes with pub/sub due to the
	 * dreaded assertion failure: !more (fq.cpp:107) from 0mq
	 */

	// how many bytes will we need for the buffer?
	std::map<std::string, std::string>::iterator metaIter;
	size_t bufferSize = msg->size();
	for (metaIter = metaKeys.begin(); metaIter != metaKeys.end(); metaIter++) {
		if (metaIter->first.size() == 0) // we do not accept empty keys!
			continue;
		bufferSize += metaIter->first.size() + 1;
		bufferSize += metaIter->second.size() + 1;
	}

	/**
	 * Message layout
	 * key1  \0  value1  \0  key2  \0  value2\0  \0
	 *                                 delimiter ^^
	 * keys cannot be empty and there will always be meta data,
	 * thus two zero bytes signify meta end
	 */
	bufferSize += 1; // delimiter

	zmq_msg_t payload;
	zmq_msg_init(&payload) && UM_LOG_WARN("zmq_msg_init: %s", zmq_strerror(errno));
	zmq_msg_init_size (&payload, bufferSize) && UM_LOG_WARN("zmq_msg_init_size: %s", zmq_strerror(errno));
	char* dataStart = (char*)zmq_msg_data(&payload);
	char* writePtr = dataStart;

	// breaks zero copy :(
	for (metaIter = metaKeys.begin(); metaIter != metaKeys.end(); metaIter++) {
		if (metaIter->first.size() == 0) // we do not accept empty keys!
			continue;
		writePtr = Message::write(metaIter->first, writePtr);
		writePtr = Message::write(metaIter->second, writePtr);
	}
	// terminate meta keys
	writePtr[0] = '\0';
	writePtr++;

	assert(bufferSize - (writePtr - dataStart) == msg->size());

	memcpy(writePtr, msg->data(), msg->size());

    zmq_sendmsg(_pubSocket, &payload, 0) >= 0 || UM_LOG_WARN("zmq_sendmsg: %s", zmq_strerror(errno));
    zmq_msg_close(&payload) && UM_LOG_WARN("zmq_msg_close: %s", zmq_strerror(errno));

#endif


}


}
