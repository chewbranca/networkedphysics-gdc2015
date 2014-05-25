/*
    Network Protocol Library
    Copyright (c) 2013-2014 Glenn Fiedler <glenn.fiedler@gmail.com>
*/

#ifndef PROTOCOL_RELIABLE_MESSAGE_CHANNEL_H
#define PROTOCOL_RELIABLE_MESSAGE_CHANNEL_H

#include "MessageChannel.h"    

namespace protocol
{
    struct ReliableMessageChannelConfig
    {
        ReliableMessageChannelConfig()
        {
            resendRate = 0.1f;
            sendQueueSize = 1024;
            receiveQueueSize = 256;
            sentPacketsSize = 256;
            maxMessagesPerPacket = 32;
            maxMessageSize = 64;
            maxSmallBlockSize = 64;
            packetBudget = 128;
            giveUpBits = 64;
        }

        float resendRate;               // message max resend rate in seconds, until acked.
        int sendQueueSize;              // send queue size in # of entries
        int receiveQueueSize;           // receive queue size in # of entries
        int sentPacketsSize;            // sent packets sliding window size in # of entries
        int maxMessagesPerPacket;       // maximum number of messages included in a packet
        int maxMessageSize;             // maximum message size allowed in iserialized bytes, eg. post bitpacker
        int maxSmallBlockSize;          // maximum small block size allowed. messages above this size are fragmented and reassembled.
        int packetBudget;               // maximum number of bytes this channel may take per-packet. 
        int giveUpBits;                 // give up trying to add more messages to packet if we have less than this # of bits available.

        // todo: move packet budget into the controlling owner, and pass it in to "GetData". should be *dynamic*

        shared_ptr<Factory<Message>> messageFactory;
    };

    class ReliableMessageChannelData : public ChannelData
    {
        const ReliableMessageChannelConfig * config;

    public:

        ReliableMessageChannelData( const ReliableMessageChannelConfig & _config ) : config( &_config ) {}

        vector<shared_ptr<Message>> messages;

        void Serialize( Stream & stream )
        {
            assert( config );
            assert( config->messageFactory );           // IMPORTANT: You must supply a message factory!

            int numMessages = stream.IsWriting() ? messages.size() : 0;
            serialize_int( stream, numMessages, 0, config->maxMessagesPerPacket );
            if ( stream.IsReading() )
                messages.resize( numMessages );

            for ( int i = 0; i < numMessages; ++i )
            {
                #ifndef NDEBUG
                if ( stream.IsWriting() )
                    assert( messages[i] );
                #endif

                int messageType = stream.IsWriting() ? messages[i]->GetType() : 0;
                serialize_int( stream, messageType, 0, config->messageFactory->GetMaxType() );
                if ( stream.IsReading() )
                {
                    messages[i] = config->messageFactory->Create( messageType );
                    assert( messages[i] );
                    assert( messages[i]->GetType() == messageType );
                }

                uint16_t messageId = stream.IsWriting() ? messages[i]->GetId() : 0;
                serialize_bits( stream, messageId, 16 );
                if ( stream.IsReading() )
                {
                    messages[i]->SetId( messageId );
                    assert( messages[i]->GetId() == messageId );
                }

                messages[i]->Serialize( stream );
            }
        }
    };

    class ReliableMessageChannel : public MessageChannel
    {
        struct SendQueueEntry
        {
            shared_ptr<Message> message;
            uint32_t sequence : 16;                      // this is actually the message id      
            uint32_t valid : 1;
            uint32_t measuredBits : 15;
            double timeLastSent;
            SendQueueEntry()
                : valid(0) {}
            SendQueueEntry( shared_ptr<Message> _message, uint16_t _sequence )
                : message( _message ), sequence( _sequence ), valid(1), measuredBits(0), timeLastSent(-1) {}
        };

        struct SentPacketEntry
        {
            uint32_t sequence : 16;                      // this is the packet sequence #
            uint32_t acked : 1;
            uint32_t valid : 1;
            double timeSent;
            vector<uint16_t> messageIds;
            SentPacketEntry() : valid(0) {}
        };

        struct ReceiveQueueEntry
        {
            shared_ptr<Message> message;
            uint32_t sequence : 16;                      // this is actually the message id      
            uint32_t valid : 1;
            double timeReceived;
            ReceiveQueueEntry()
                : valid(0) {}
            ReceiveQueueEntry( shared_ptr<Message> _message, uint16_t _sequence, double _timeReceived )
                : message( _message ), sequence( _sequence ), valid(1), timeReceived( _timeReceived ) {}
        };

    public:

        enum Counters
        {
            MessagesSent,
            MessagesWritten,
            MessagesRead,
            MessagesReceived,
            MessagesDiscardedLate,
            MessagesDiscardedEarly,
            NumCounters
        };

        ReliableMessageChannel( const ReliableMessageChannelConfig & config ) : m_config( config )
        {
            assert( config.messageFactory );
            assert( config.maxSmallBlockSize <= MaxSmallBlockSize );

            m_sendMessageId = 0;
            m_receiveMessageId = 0;

            m_sendQueue = make_shared<SlidingWindow<SendQueueEntry>>( m_config.sendQueueSize );
            m_sentPackets = make_shared<SlidingWindow<SentPacketEntry>>( m_config.sentPacketsSize );
            m_receiveQueue = make_shared<SlidingWindow<ReceiveQueueEntry>>( m_config.receiveQueueSize );

            m_counters.resize( NumCounters, 0 );

            const int SmallBlockOverhead = bits_required( 0, MaxSmallBlockSize - 1 );
            m_measureBuffer.resize( max( m_config.maxMessageSize, m_config.maxSmallBlockSize + SmallBlockOverhead ) );

//            cout << "measure buffer is " << m_measureBuffer.size() << " bytes" << endl;

            const int MessageIdBits = 16;
            const int MessageTypeBits = bits_required( 0, m_config.messageFactory->GetMaxType() );
            m_messageOverheadBits = MessageIdBits + MessageTypeBits;

//            cout << "message overhead is " << m_messageOverheadBits << " bits" << endl;
        }

        bool CanSendMessage() const
        {
            return m_sendQueue->HasSlotAvailable( m_sendMessageId );
        }

        void SendMessage( shared_ptr<Message> message )
        {
//            cout << "queue message for send: " << m_sendMessageId << endl;

            if ( !m_sendQueue->HasSlotAvailable( m_sendMessageId ) )
                throw runtime_error( "message send queue overflow" );

            message->SetId( m_sendMessageId );

            bool result = m_sendQueue->Insert( SendQueueEntry( message, m_sendMessageId ) );
            assert( result );

            #ifndef NDEBUG
            auto entry = m_sendQueue->Find( m_sendMessageId );
            assert( entry );
            assert( entry->valid );
            assert( entry->sequence == m_sendMessageId );
            assert( entry->message );
            assert( entry->message->GetId() == m_sendMessageId );
            #endif

            // todo: would be nice to have a specialized measure stream that doesn't actually do any work
            // but measure the # of bits required for a specific serialization
            Stream stream( STREAM_Write, &m_measureBuffer[0], m_measureBuffer.size() );
            message->Serialize( stream );
            entry->measuredBits = stream.GetBits() + m_messageOverheadBits;
//            cout << "message " << m_sendMessageId << " is " << entry->measuredBits << " bits " << endl;

            m_counters[MessagesSent]++;

            m_sendMessageId++;
        }

        void SendBlock( shared_ptr<Block> block )
        {
//            cout << "send block: " << block->size() << " bytes" << endl;

            // todo: large block support
            assert( block->size() <= m_config.maxSmallBlockSize );

            auto blockMessage = make_shared<BlockMessage>( block );

            SendMessage( blockMessage );
        }        

        shared_ptr<Message> ReceiveMessage()
        {
            auto entry = m_receiveQueue->Find( m_receiveMessageId );
            if ( !entry )
                return nullptr;

            auto message = entry->message;

            #ifndef NDEBUG
            assert( message );
            assert( message->GetId() == m_receiveMessageId );
            #endif

//            cout << "dequeue for receive: " << message->GetId() << endl;

            entry->valid = 0;
            entry->message = nullptr;
            
            m_counters[MessagesReceived]++;

            m_receiveMessageId++;

            return message;
        }

        shared_ptr<ChannelData> CreateData()
        {
            return make_shared<ReliableMessageChannelData>( m_config );
        }

        shared_ptr<ChannelData> GetData( uint16_t sequence )
        {
            // find oldest message id in send queue

            bool foundMessage = false;
            uint16_t oldestMessageId = 0;
            const uint16_t baseId = m_sendMessageId - m_config.sendQueueSize;
            for ( int i = 0; i < m_config.sendQueueSize; ++i )
            {
                const uint16_t messageId = baseId + i;
                SendQueueEntry * entry = m_sendQueue->Find( messageId );
                if ( entry )
                {
                    if ( !foundMessage || sequence_less_than( messageId, oldestMessageId ) )
                    {
                        oldestMessageId = messageId;
                        foundMessage = true;
                    }
                }
            }

            // if we didn't find any messages, there is no data to send

            if ( !foundMessage )
                return nullptr;

            // gather messages to include in the packet

            // todo: really would prefer if budget was passed in as # of bits from connection
            int availableBits = m_config.packetBudget * 8;

            int numMessageIds = 0;
            uint16_t messageIds[m_config.maxMessagesPerPacket];
            for ( int i = 0; i < m_config.receiveQueueSize; ++i )
            {
                // IMPORTANT: Give up if we don't have a lot of bits available
                // Otherwise we can search in vain across entire send queue for
                // a message that cannot possibly fit in the bits we have left.
                if ( availableBits < m_config.giveUpBits )
                    break;

                const uint16_t messageId = oldestMessageId + i;
                SendQueueEntry * entry = m_sendQueue->Find( messageId );
                if ( entry && entry->timeLastSent + m_config.resendRate <= m_timeBase.time && availableBits - entry->measuredBits >= 0 )
                {
                    messageIds[numMessageIds++] = messageId;
                    entry->timeLastSent = m_timeBase.time;
                    availableBits -= entry->measuredBits;
                }
                if ( numMessageIds == m_config.maxMessagesPerPacket )
                    break;
            }
            assert( numMessageIds >= 0 );
            assert( numMessageIds <= m_config.maxMessagesPerPacket );

//            cout << "wrote " << numMessageIds << " of " << m_config.maxMessagesPerPacket << " maximum messages, using " << m_config.packetBudget * 8 - availableBits << " of " << m_config.packetBudget * 8 << " available bits" << endl;

            // if there are no messages then we don't have any data to send

            if ( numMessageIds == 0 )
                return nullptr;

            // add sent packet data containing message ids included in this packet

            auto sentPacketData = m_sentPackets->InsertFast( sequence );
            assert( sentPacketData );
            sentPacketData->acked = 0;
            sentPacketData->timeSent = m_timeBase.time;
            sentPacketData->messageIds.resize( numMessageIds );
            for ( int i = 0; i < numMessageIds; ++i )
                sentPacketData->messageIds[i] = messageIds[i];

            // update counter: num messages written

            m_counters[MessagesWritten] += numMessageIds;

            // construct channel data for packet

            auto data = make_shared<ReliableMessageChannelData>( m_config );

            data->messages.resize( numMessageIds );
            for ( int i = 0; i < numMessageIds; ++i )
            {
                auto entry = m_sendQueue->Find( messageIds[i] );
                assert( entry );
                assert( entry->message );
                data->messages[i] = entry->message;
            }

            return data;
        }

        void ProcessData( uint16_t sequence, shared_ptr<ChannelData> channelData )
        {
            assert( channelData );

            auto data = reinterpret_cast<ReliableMessageChannelData&>( *channelData );

//            cout << "process message channel data: " << sequence << endl;

            bool earlyMessage = false;

            const uint16_t minMessageId = m_receiveMessageId;
            const uint16_t maxMessageId = m_receiveMessageId + m_config.receiveQueueSize - 1;

            // process messages included in this packet data

//            cout << data.messages.size() << " messages in packet" << endl;

            for ( auto message : data.messages )
            {
                assert( message );

                const uint16_t messageId = message->GetId();

//                cout << "add message to receive queue: " << messageId << endl;

                if ( sequence_less_than( messageId, minMessageId ) )
                {
//                    cout << "old message " << messageId << endl;

                    m_counters[MessagesDiscardedLate]++;
                }
                else if ( sequence_greater_than( messageId, maxMessageId ) )
                {
//                    cout << "early message " << messageId << endl;

                    earlyMessage = true;

                    m_counters[MessagesDiscardedEarly]++;
                }
                else
                {
                    bool result = m_receiveQueue->Insert( ReceiveQueueEntry( message, messageId, m_timeBase.time ) );

                    assert( result );
                }

                m_counters[MessagesRead]++;
            }

            if ( earlyMessage )
                throw runtime_error( "received early message" );
        }

        void ProcessAck( uint16_t ack )
        {
//            cout << "process ack: " << ack << endl;

            auto sentPacket = m_sentPackets->Find( ack );
            if ( !sentPacket || sentPacket->acked )
                return;

            for ( auto messageId : sentPacket->messageIds )
            {
                auto sendQueueEntry = m_sendQueue->Find( messageId );
                if ( sendQueueEntry )
                {
                    assert( sendQueueEntry->message );
                    assert( sendQueueEntry->message->GetId() == messageId );

//                    cout << "acked message " << messageId << endl;

                    sendQueueEntry->valid = 0;
                    sendQueueEntry->message = nullptr;
                }
            }

            sentPacket->acked = 1;
        }

        void Update( const TimeBase & timeBase )
        {
            m_timeBase = timeBase;
        }

        uint64_t GetCounter( int index ) const
        {
            assert( index >= 0 );
            assert( index < NumCounters );
            return m_counters[index];
        }

    private:

        ReliableMessageChannelConfig m_config;                              // constant configuration data
        TimeBase m_timeBase;                                                // current time base from last update
        uint16_t m_sendMessageId;                                           // id for next message added to send queue
        uint16_t m_receiveMessageId;                                        // id for next message to be received
        shared_ptr<SlidingWindow<SendQueueEntry>> m_sendQueue;              // message send queue
        shared_ptr<SlidingWindow<SentPacketEntry>> m_sentPackets;           // sent packets (for acks)
        shared_ptr<SlidingWindow<ReceiveQueueEntry>> m_receiveQueue;        // message receive queue
        vector<uint64_t> m_counters;                                        // counters        
        vector<uint8_t> m_measureBuffer;                                    // buffer used for measuring message size in bits
        int m_messageOverheadBits;                                          // number of bits overhead per-serialized message
    };

}

#endif