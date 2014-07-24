/*
    Network Protocol Foundation Library.
    Copyright (c) 2014, The Network Protocol Company, Inc.
*/

#include "DataBlockReceiver.h"
#include "Allocator.h"

namespace protocol
{
    DataBlockReceiver::DataBlockReceiver( Allocator & allocator, int fragmentSize, int maxBlockSize )
    {
        PROTOCOL_ASSERT( fragmentSize > 0 );
        PROTOCOL_ASSERT( fragmentSize <= MaxFragmentSize );
        PROTOCOL_ASSERT( maxBlockSize > 0 );

        m_allocator = &allocator;
        m_data = (uint8_t*) allocator.Allocate( maxBlockSize );
        m_maxBlockSize = maxBlockSize;
        m_fragmentSize = fragmentSize;
        m_maxFragments = m_maxBlockSize / m_fragmentSize + ( ( m_maxBlockSize % m_fragmentSize ) ? 1 : 0 );
        m_receivedFragment = (uint8_t*) allocator.Allocate( m_maxFragments );

        Clear();
    }

    DataBlockReceiver::~DataBlockReceiver()
    {
        PROTOCOL_ASSERT( m_allocator );
        PROTOCOL_ASSERT( m_data );
        PROTOCOL_ASSERT( m_receivedFragment );

        m_block.Disconnect();

        m_allocator->Free( m_data );
        m_allocator->Free( m_receivedFragment );

        m_allocator = nullptr;
        m_data = nullptr;
        m_receivedFragment = nullptr;
    }

    void DataBlockReceiver::Clear()
    {
        m_blockSize = 0;
        m_numFragments = 0;
        m_numReceivedFragments = 0;
        m_error = 0;
        m_block.Disconnect();
        memset( m_receivedFragment, 0, m_maxFragments );
    }

    void DataBlockReceiver::ProcessFragment( int blockSize, int numFragments, int fragmentId, int fragmentBytes, uint8_t * fragmentData )
    {
        if ( blockSize > m_maxBlockSize )
        {
            m_error = DATA_BLOCK_RECEIVER_ERROR_BLOCK_TOO_LARGE;
            return;
        }

        if ( m_error != 0 )
            return;

        // 1. intensive validation!

        if ( m_blockSize == 0 )
            m_blockSize = blockSize;

        if ( m_blockSize != blockSize )
            return;

        if ( m_numFragments > m_maxFragments )
            return;

        if ( m_numFragments == 0 )
            m_numFragments = numFragments;

        if ( m_numFragments != numFragments )
            return;

        if ( fragmentId >= m_numFragments )
            return;

        const int start = fragmentId * m_fragmentSize;
        const int finish = start + fragmentBytes;

        PROTOCOL_ASSERT( finish <= m_blockSize );
        if ( finish > m_blockSize )
            return;

        // 2. send an ack and process the fragment

        SendAck( fragmentId );

        if ( !m_receivedFragment[fragmentId] )
        {
            m_receivedFragment[fragmentId] = 1;
            m_numReceivedFragments++;

            PROTOCOL_ASSERT( m_numReceivedFragments >= 0 );
            PROTOCOL_ASSERT( m_numReceivedFragments <= m_numFragments );

            memcpy( m_data + start, fragmentData, fragmentBytes );
        }
    }

    Block * DataBlockReceiver::GetBlock()
    {
        if ( ReceiveCompleted() && m_blockSize > 0 )
        {
            m_block.Disconnect();
            m_block.Connect( *m_allocator, m_data, m_blockSize );
            return &m_block;
        }
        else
            return nullptr;
    }
}
