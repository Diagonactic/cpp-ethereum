
#include "EthStratumClient.h"

using boost::asio::ip::tcp;


EthStratumClient::EthStratumClient(GenericFarm<EthashProofOfWork> * f, MinerType m, string const & host, string const & port, string const & user, string const & pass)
	: m_socket(m_io_service)
{
	m_minerType = m;
	m_host = host;
	m_port = port;
	m_user = user;
	m_pass = pass;
	m_authorized = false;
	m_connected = false;
	m_precompute = true;
	m_pending = 0;
	

	p_farm = f;
	connect();
}

EthStratumClient::~EthStratumClient()
{

}

void EthStratumClient::connect()
{
	
	tcp::resolver r(m_io_service);
	tcp::resolver::query q(m_host, m_port);
	
	r.async_resolve(q, boost::bind(&EthStratumClient::resolve_handler,
																	this, boost::asio::placeholders::error,
																	boost::asio::placeholders::iterator));
	
	cnote << "Connecting to stratum server " << m_host +":"+m_port;

	boost::thread t(boost::bind(&boost::asio::io_service::run, &m_io_service));
	
}

void EthStratumClient::reconnect()
{
	if (p_farm->isMining())
	{
		cnote << "Stopping farm";
		p_farm->stop();
	}
	m_socket.close();
	m_io_service.reset();
	m_authorized = false;
	m_connected = false;
	cnote << "Reconnecting in 3 seconds...";
	boost::asio::deadline_timer     timer(m_io_service, boost::posix_time::seconds(3));
	timer.wait();
	connect();
}

void EthStratumClient::disconnect()
{
	cnote << "Disconnecting";
	m_connected = false;
	if (p_farm->isMining())
	{
		cnote << "Stopping farm";
		p_farm->stop();
	}
	m_socket.close();
	m_io_service.stop();
}

void EthStratumClient::resolve_handler(const boost::system::error_code& ec, tcp::resolver::iterator i)
{
	if (!ec)
	{
		async_connect(m_socket, i, boost::bind(&EthStratumClient::connect_handler,
																					this, boost::asio::placeholders::error,
																					boost::asio::placeholders::iterator));
	}
	else
	{
		cerr << "Could not resolve host" << m_host + ":" + m_port + ", " << ec.message();
		reconnect();
	}
}

void EthStratumClient::connect_handler(const boost::system::error_code& ec, tcp::resolver::iterator i)
{
	if (!ec)
	{
		m_connected = true;
		cnote << "Connected to stratum server " << m_host << ":" << m_port;
		cnote << "Starting farm";
		if (m_minerType == MinerType::CPU)
			p_farm->start("cpu");
		else if (m_minerType == MinerType::CL)
			p_farm->start("opencl");
		else if (m_minerType == MinerType::CUDA)
			p_farm->start("cuda");

		std::ostream os(&m_requestBuffer);
		os << "{\"id\": 1, \"method\": \"mining.subscribe\", \"params\": []}\n";

		
		async_write(m_socket, m_requestBuffer,
			boost::bind(&EthStratumClient::handleResponse, this,
									boost::asio::placeholders::error));
	}
	else
	{
		cwarn << "Could not connect to stratum server " << m_host << ":" << m_port << ", " << ec.message();
		reconnect();
	}

}

void EthStratumClient::readline() {

	if (m_pending == 0) {
		async_read_until(m_socket, m_responseBuffer, "\n",
			boost::bind(&EthStratumClient::readResponse, this,
			boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
		m_pending++;
	}
}

void EthStratumClient::handleResponse(const boost::system::error_code& ec) {
	if (!ec)
	{
		readline();
	}
	else
	{
		cwarn << "Handle response failed: " << ec.message();
	}
}

void EthStratumClient::readResponse(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
	m_pending = m_pending > 0 ? m_pending - 1 : 0;

	if (!ec)
	{
		std::istream is(&m_responseBuffer);
		std::string response;
		getline(is, response);

		if (response.front() == '{' && response.back() == '}') 
		{
			Json::Value responseObject;
			Json::Reader reader;
			if (reader.parse(response.c_str(), responseObject))
			{
				processReponse(responseObject);
				m_response = response;
			}
			else
			{
				cwarn << "Parse response failed: " << reader.getFormattedErrorMessages();
			}
		}
		else
		{
			cwarn << "Discarding incomplete response";
		}
		readline();
	}
	else
	{
		cwarn << "Read response failed: " << ec.message();
		reconnect();
	}
}

void EthStratumClient::processReponse(Json::Value& responseObject)
{
	Json::Value error = responseObject.get("error", new Json::Value);
	if (error.isArray())
	{
		string msg = error.get(1, "Unknown error").asString();
		cnote << msg;
	}
	std::ostream os(&m_requestBuffer);
	Json::Value params;
	int id = responseObject.get("id", Json::Value::null).asInt();
	switch (id)
	{
	case 1:
		cnote << "Subscribed to stratum server";

		os << "{\"id\": 2, \"method\": \"mining.authorize\", \"params\": [\"" << m_user << "\",\"" << m_pass << "\"]}\n";

		async_write(m_socket, m_requestBuffer,
			boost::bind(&EthStratumClient::handleResponse, this,
			boost::asio::placeholders::error));
		break;
	case 2:
		m_authorized = responseObject.get("result", Json::Value::null).asBool();
		if (!m_authorized)
		{
			disconnect();
			return;
		}
		cnote << "Authorized worker " << m_user;
		break;
	case 4:
 		if (responseObject.get("result", false).asBool())
			cnote << "B-) Submitted and accepted.";
		else
			cwarn << ":-( Not accepted.";
		break;
	default:
		string method = responseObject.get("method", "").asString();
		if (method == "mining.notify")
		{
			params = responseObject.get("params", Json::Value::null);
			if (params.isArray())
			{
				m_job = params.get((Json::Value::ArrayIndex)0, "").asString();
				string sHeaderHash = params.get((Json::Value::ArrayIndex)1, "").asString();
				string sSeedHash = params.get((Json::Value::ArrayIndex)2, "").asString();
				string sShareTarget = params.get((Json::Value::ArrayIndex)3, "").asString();
				bool cleanJobs = params.get((Json::Value::ArrayIndex)4, "").asBool();

				if (sShareTarget.length() < 66)
					sShareTarget = "0x" + string(66 - sShareTarget.length(), '0') + sShareTarget.substr(2);

				int l = sShareTarget.length();

				if (sHeaderHash != "" && sSeedHash != "" && sShareTarget != "")
				{
					cnote << "Received new job #" + m_job;
					cnote << "Header hash: " + sHeaderHash;
					cnote << "Seed hash: " + sSeedHash;
					cnote << "Share target: " + sShareTarget;

					h256 seedHash = h256(sSeedHash);
					h256 headerHash = h256(sHeaderHash);
					EthashAux::FullType dag;


					if (seedHash != m_current.seedHash)
						cnote << "Grabbing DAG for" << seedHash;
					if (!(dag = EthashAux::full(seedHash, true, [&](unsigned _pc){ cout << "\rCreating DAG. " << _pc << "% done..." << flush; return 0; })))
						BOOST_THROW_EXCEPTION(DAGCreationFailure());
					if (m_precompute)
						EthashAux::computeFull(sha3(seedHash), true);
					if (headerHash != m_current.headerHash)
					{
						m_current.headerHash = h256(sHeaderHash);
						m_current.seedHash = seedHash;
						m_current.boundary = h256(sShareTarget);// , h256::AlignRight);
						p_farm->setWork(m_current);
					}
				}
			}
		}
		else if (method == "mining.set_difficulty")
		{

		}
		else if (method == "client.get_version")
		{
			os << "{\"error\": null, \"id\" : " << id << ", \"result\" : \"" << ETH_PROJECT_VERSION << "\"}\n";
			async_write(m_socket, m_requestBuffer,
				boost::bind(&EthStratumClient::handleResponse, this,
				boost::asio::placeholders::error));
		}
		break;
	}

}

bool EthStratumClient::submit(EthashProofOfWork::Solution solution) {

	cnote << "Solution found; Submitting to" << m_host << "...";
	cnote << "  Nonce:" << "0x"+solution.nonce.hex();
	cnote << "  Mixhash:" << "0x" + solution.mixHash.hex();
	cnote << "  Header-hash:" << "0x" + m_current.headerHash.hex();
	cnote << "  Seedhash:" << "0x" + m_current.seedHash.hex();
	cnote << "  Target: " << "0x" + h256(m_current.boundary).hex();
	cnote << "  Ethash: " << "0x" + h256(EthashAux::eval(m_current.seedHash, m_current.headerHash, solution.nonce).value).hex();
	if (EthashAux::eval(m_current.seedHash, m_current.headerHash, solution.nonce).value < m_current.boundary)
	{
		string json = "{\"id\": 4, \"method\": \"mining.submit\", \"params\": [\"" + m_user + "\",\"" + m_job + "\",\"0x" + solution.nonce.hex() + "\",\"0x" + m_current.headerHash.hex() + "\",\"0x" + solution.mixHash.hex() + "\"]}\n";
		std::ostream os(&m_requestBuffer);
		os << json;

		async_write(m_socket, m_requestBuffer,
			boost::bind(&EthStratumClient::handleResponse, this,
			boost::asio::placeholders::error));
		return true;
	}
	else
		cwarn << "FAILURE: GPU gave incorrect result!";
	
	return false;
}

