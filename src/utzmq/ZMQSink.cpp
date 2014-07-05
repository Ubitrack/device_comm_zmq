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

/**
 * @ingroup driver_components
 * @file
 * NetworkSink component.
 *
 * @author Florian Echtler <echtler@in.tum.de>
 */

// WARNING: all boost/serialization headers should be
//          included AFTER all boost/archive headers

#ifdef ___COMMENTED_OUT


#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/utility.hpp>

#include <string>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <iomanip>

// on windows, asio must be included before anything that possible includes windows.h
// don't ask why.
#include <boost/asio.hpp>

#include <boost/bind.hpp>
#include <boost/thread.hpp>

#include <boost/array.hpp>
#include <boost/thread.hpp>

#include <utDataflow/PushConsumer.h>
#include <utDataflow/ComponentFactory.h>
#include <utDataflow/Component.h>
#include <utMeasurement/Measurement.h>

namespace Ubitrack { namespace Drivers {

static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.NetworkSink" ) );
	
/**
 * @ingroup dataflow_components
 * Transmits measurements over the network.
 *
 * @par Input Ports
 * PushConsumer<EventType> port "Input"
 *
 * @par Output Ports
 * None.
 *
 * @par Configuration
 * - Edge configuration:
 * @verbatim
 * <Configuration port="..." destination="..."/>
 * @endverbatim
 *   - \c port: the UDP target port, default 0x5554 ("UT")
 *   - \c destination: the target machine or ip, default 127.0.0.1
 *
 * @par Instances
 * Registered for the following EventTypes and names:
 * - Ubitrack::Measurement::Position: NetworkPositionSink
 * - Ubitrack::Measurement::Rotation: NetworkRotationSink
 * - Ubitrack::Measurement::Pose : NetworkPoseSink
 */
template< class EventType >
class SinkComponent
	: public Dataflow::Component
{

public:

	/** constructor */
	SinkComponent( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > pConfig )
		: Dataflow::Component( name )
		, m_inPort( "Input", *this, boost::bind( &SinkComponent::eventIn, this, _1 ) )
		, m_IoService()
		, m_UDPPort( 0x5554 ) // default port is 0x5554 (UT)
		, m_Destination( "127.0.0.1" )
	{
		using boost::asio::ip::udp;

		// check for configuration
		pConfig->m_DataflowAttributes.getAttributeData( "networkPort", m_UDPPort );
		if ( pConfig->m_DataflowAttributes.hasAttribute( "networkDestination" ) )
		{
			m_Destination = pConfig->m_DataflowAttributes.getAttributeString( "networkDestination" );
		}

		// open new socket which we use for sending stuff
		m_SendSocket = boost::shared_ptr< udp::socket >( new udp::socket (m_IoService) );
		m_SendSocket->open( udp::v4() );

		// resolve destination pair and store the remote endpoint
		udp::resolver resolver( m_IoService );

		std::ostringstream portString;
		portString << m_UDPPort;

		udp::resolver::query query( udp::v4(), m_Destination, portString.str() );
		m_SendEndpoint = boost::shared_ptr< udp::endpoint >( new udp::endpoint( *resolver.resolve( query ) ) );
	}

protected:

	// receive a new pose from the dataflow
	void eventIn( const EventType& m )
	{
		std::ostringstream stream;
		boost::archive::text_oarchive packet( stream );

		std::string suffix("\n");
		Measurement::Timestamp sendtime;

		// serialize the measurement, component name and current local time
		packet << m_name;
		packet << m;
		sendtime = Measurement::now();
		packet << sendtime;
		packet << suffix;

		m_SendSocket->send_to( boost::asio::buffer( stream.str().c_str(), stream.str().size() ), *m_SendEndpoint );
	}

	// consumer port
	Dataflow::PushConsumer< EventType > m_inPort;

	boost::asio::io_service m_IoService;

	boost::shared_ptr< boost::asio::ip::udp::socket > m_SendSocket;
	boost::shared_ptr< boost::asio::ip::udp::endpoint > m_SendEndpoint;

	int m_UDPPort;
	std::string m_Destination;
};


// register module at factory
UBITRACK_REGISTER_COMPONENT( Dataflow::ComponentFactory* const cf ) {
	cf->registerComponent< SinkComponent< Measurement::Pose > > ( "NetworkPoseSink" );
	cf->registerComponent< SinkComponent< Measurement::ErrorPose > > ( "NetworkErrorPoseSink" );
	cf->registerComponent< SinkComponent< Measurement::Position > > ( "NetworkPositionSink" );
	cf->registerComponent< SinkComponent< Measurement::Position2D > > ( "NetworkPosition2DSink" );
	cf->registerComponent< SinkComponent< Measurement::Rotation > > ( "NetworkRotationSink" );
	cf->registerComponent< SinkComponent< Measurement::PoseList > > ( "NetworkPoseListSink" );
	cf->registerComponent< SinkComponent< Measurement::PositionList > > ( "NetworkPositionListSink" );
	cf->registerComponent< SinkComponent< Measurement::PositionList2 > > ( "NetworkPositionList2Sink" );
	cf->registerComponent< SinkComponent< Measurement::Button > > ( "NetworkEventSink" );
	cf->registerComponent< SinkComponent< Measurement::Matrix3x3 > > ( "NetworkMatrix3x3Sink" );
	cf->registerComponent< SinkComponent< Measurement::Matrix3x4 > > ( "NetworkMatrix3x4Sink" );
	cf->registerComponent< SinkComponent< Measurement::Matrix4x4 > > ( "NetworkMatrix4x4Sink" );
}

} } // namespace Ubitrack::Drivers

#endif // COMMENTED_OUT