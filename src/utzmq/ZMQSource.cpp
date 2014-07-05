/*
 * Ubitrack - Library for Ubiquitous Tracking
 * Copyright 2006, Technische Universitaet Muenchen, and individual
 * contributors as indicated by the @authors tag. See the
 * copyright.txt in the distribution for a full listing of individual
 * contributors.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this software; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA, or see the FSF site: http://www.fsf.org.
 */

#include <boost/archive/text_iarchive.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/binary_object.hpp>

#include "ZMQSource.h"

#include <iostream>
#include <sstream>

#include <utDataflow/ComponentFactory.h>
#include <utMeasurement/Measurement.h>

#include <boost/array.hpp>

#include <log4cpp/Category.hh>


namespace Ubitrack { namespace Drivers {

static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.ZMQSource" ) );

SourceModule::SourceModule( const SourceModuleKey& moduleKey, boost::shared_ptr< Graph::UTQLSubgraph >, FactoryHelper* pFactory )
	: Module< SourceModuleKey, SourceComponentKey, SourceModule, SourceComponentBase >( moduleKey, pFactory )
	, context(NULL)
	, signals_socket(NULL)
	, m_io_threads(1)
	, m_signals_url(ZMQSOURCE_SIGNALS_URL)
{
	stopModule();
}


SourceModule::~SourceModule()
{
	stopModule();
}

void SourceModule::startModule()
{
	if ( !m_running )
	{
		m_running = true;

		LOG4CPP_DEBUG( logger, "Starting ZMQ Source module" );
//		LOG4CPP_DEBUG( logger, "Creating receiver on port " << m_moduleKey );

        context = new zmq::context_t(m_io_threads);

        try {
            signals_socket->bind(m_signals_url.c_str());
        } catch (zmq::error_t &e) {
            delete signals_socket;
            signals_socket = NULL;
        }



//		m_Socket->async_receive_from(
//			boost::asio::buffer( receive_data, max_receive_length ),
//		sender_endpoint,
//		boost::bind( &SourceModule::HandleReceive, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred ) );
//
//		// network thread runs until io_service is interrupted
//		LOG4CPP_DEBUG( logger, "Starting network receiver thread" );
//		m_NetworkThread = boost::shared_ptr< boost::thread >( new boost::thread( boost::bind( &boost::asio::io_service::run, m_IoService.get() ) ) );

		LOG4CPP_DEBUG( logger, "ZMQ Source module started" );
	}
}

void SourceModule::stopModule()
{

	if ( m_running )
	{
		m_running = false;
		LOG4CPP_NOTICE( logger, "Stopping ZMQ Source Module" );

        // do something

	}
	LOG4CPP_DEBUG( logger, "ZMQ Source Stopped" );
}


zmq::socket_t* SourceModule::createSocket(int type) {
    return new zmq::socket_t(*context, type);
}


int SourceModule::sendSignal(unsigned int signal, unsigned int timestamp) {
/*
        if (!signals_socket) {
                return -1;
        }

        MessageHeader::Signals header;

        header.set_signal(signal);
        header.set_timestamp(timestamp);
        int header_size = header.ByteSize();
        //prefix int(num bytes headers), headers_bytes + std:ends
        zmq::message_t message(sizeof(int32_t) + header_size + 1);
        int msg_offset = 0;

        int32_t header_size_i32 = htonl (header_size);
        memcpy ((char *)message.data(), &header_size_i32, sizeof (int32_t));
        msg_offset += sizeof (int32_t);
        char* message_buf = (char *)(message.data());

        ostreambuf<char> msg_obuffer((char *)(message.data())+msg_offset, message.size()-msg_offset);
        std::ostream msg_ostream(&msg_obuffer);
        try {
                header.SerializeToOstream(&msg_ostream);
                msg_offset += header.ByteSize();
                message_buf[msg_offset] = 0;
                msg_offset += 1;
        }
        catch (...) {
                ostringstream log;
                log << "error serializing header: " << std::endl;
                log << header.DebugString() << std::endl;
                H3DZMQ_DEBUG_LOG_I(log.str());

                // free any allocated vars here !!!
                return -1;
        }

        try {
                signals_socket->send(message, ZMQ_NOBLOCK);
        }catch (...) {
                ostringstream log;
                log << "error sending signal: " << std::endl;
                log << header.DebugString() << std::endl;
                H3DZMQ_DEBUG_LOG_I(log.str());

                // free any allocated vars here !!!
                return -1;
        }
*/
        return 0;
}


//void SourceModule::HandleReceive( const boost::system::error_code err, size_t length )
//{
//	Measurement::Timestamp recvtime = Measurement::now();
//
//	LOG4CPP_DEBUG( logger, "Received " << length << " bytes" );
//
//	// some error checking
//	if ( err && err != boost::asio::error::message_size )
//	{
//		std::ostringstream msg;
//		msg << "Error receiving from socket: \"" << err << "\"";
//		LOG4CPP_ERROR( logger, msg.str() );
//		UBITRACK_THROW( msg.str() );
//	}
//
//	if (length >= max_receive_length)
//	{
//		LOG4CPP_ERROR( logger, "Too many bytes received" );
//		UBITRACK_THROW( "FIXME: received more than max_receive_length bytes." );
//	}
//
//	try
//	{
//		// make receive data null terminated and create a string stream
//		receive_data[length] = 0;
//		std::string data( receive_data );
//		LOG4CPP_TRACE( logger, "data: " << data );
//		std::istringstream stream( data );
//		boost::archive::text_iarchive message( stream );
//
//		// parse packet
//		std::string name;
//		message >> name;
//		LOG4CPP_DEBUG( logger, "Message for component " << name );
//
//		SourceComponentKey key( name );
//
//		if ( hasComponent( key ) ) {
//			boost::shared_ptr< SourceComponentBase > comp = getComponent( key );
//			comp->parse( message, recvtime );
//		}
//		else
//			LOG4CPP_WARN( logger, "NetworkSink is sending with id=\"" << name << "\", found no corresponding NetworkSource pattern with same id."  );
//	}
//	catch ( const std::exception& e )
//	{
//		LOG4CPP_ERROR( logger, "Caught exception " << e.what() );
//	}
//
//	// restart receiving new packet
//	m_Socket->async_receive_from(
//		boost::asio::buffer( receive_data, max_receive_length ),
//		sender_endpoint,
//		boost::bind( &SourceModule::HandleReceive, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred )
//	);
//}


boost::shared_ptr< SourceComponentBase > SourceModule::createComponent( const std::string& type, const std::string& name,
	boost::shared_ptr< Graph::UTQLSubgraph > config, const SourceModule::ComponentKey& key, SourceModule* pModule )
{
	if ( type == "ZMQSourcePose" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Pose >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceErrorPose" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::ErrorPose >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceRotation" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Rotation >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePosition" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Position >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePosition2D" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Position2D >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePoseList" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::PoseList >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePositionList" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::PositionList >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePositionList2" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::PositionList2 >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceEvent" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Button >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceMatrix3x3" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Matrix3x3 >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceMatrix3x4" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Matrix3x4 >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceMatrix4x4" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Matrix4x4 >( name, config, key, pModule ) );

	UBITRACK_THROW( "Class " + type + " not supported by network source module." );
}


// register module at factory
UBITRACK_REGISTER_COMPONENT( Dataflow::ComponentFactory* const cf ) {

	// create list of supported types
	std::vector< std::string > sourceComponents;

	sourceComponents.push_back( "ZMQSourcePose" );
	sourceComponents.push_back( "ZMQSourceErrorPose" );
	sourceComponents.push_back( "ZMQSourceRotation" );
	sourceComponents.push_back( "ZMQSourcePosition" );
	sourceComponents.push_back( "ZMQSourcePosition2D" );
	sourceComponents.push_back( "ZMQSourcePoseList" );
	sourceComponents.push_back( "ZMQSourcePositionList" );
	sourceComponents.push_back( "ZMQSourcePositionList2" );
	sourceComponents.push_back( "ZMQSourceEvent" );
	sourceComponents.push_back( "ZMQSourceMatrix3x3" );
	sourceComponents.push_back( "ZMQSourceMatrix3x4" );
	sourceComponents.push_back( "ZMQSourceMatrix4x4" );

	cf->registerModule< SourceModule >( sourceComponents );
}

} } // namespace Ubitrack::Drivers
