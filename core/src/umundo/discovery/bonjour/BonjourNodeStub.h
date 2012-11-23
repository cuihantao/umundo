/**
 *  @file
 *  @brief      Remote Bonjour Node representation.
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

#ifndef BonjourNodeStub_H_WRJ8277D
#define BonjourNodeStub_H_WRJ8277D

#include "dns_sd.h"

#include "umundo/connection/Node.h"
#include "umundo/thread/Thread.h"

namespace umundo {

/**
 * Concrete nodestub implementor for bonjour.
 */
class DLLEXPORT BonjourNodeStub : public NodeStubImpl {
public:
	BonjourNodeStub();
	virtual ~BonjourNodeStub();

	/// Overwritten from EndPoint.
	virtual const std::string getIP() const;

private:
	bool _isAdded;
	map<int, string> _interfacesIPv4;
	map<int, string> _interfacesIPv6;

	std::set<string> _domains;
	std::set<int> _interfaceIndices;
	string _regType;
	string _fullname;

	map<string, DNSServiceRef> _serviceResolveClients; ///< Bonjour handle for node/service resolving per domain.
	map<int, DNSServiceRef> _addrInfoClients; ///< Bonjour handle for address info per interface.

	friend std::ostream& operator<<(std::ostream&, const BonjourNodeStub*);
	friend class BonjourNodeDiscovery;
};

}

#endif /* end of include guard: BonjourNodeStub_H_WRJ8277D */
