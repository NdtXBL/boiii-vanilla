#include <std_include.hpp>
#include "../services.hpp"

namespace demonware
{
	static uint8_t bandwidth_iw6[51] =
	{
		0x0F, 0xC1, 0x1C, 0x37, 0xB8, 0xEF, 0x7C, 0xD6, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0xF4, 0x01, 0x00, 0x00, 0xD0, 0x07,
		0x00, 0x00, 0x10, 0x27, 0x00, 0x00, 0x88, 0x13, 0x00, 0x00, 0xF4, 0x01,
		0x00, 0x00, 0x02, 0x0C, 0x88, 0xB3, 0x04, 0x65, 0x89, 0xBF, 0xC3, 0x6A,
		0x27, 0x94, 0xD4, 0x8F
	};

	bdBandwidthTest::bdBandwidthTest() : service(18, "bdBandwidthTest")
	{
	}

	void bdBandwidthTest::exec_task(service_server* server, const std::string& data)
	{
		byte_buffer buffer;
		buffer.write(sizeof bandwidth_iw6, bandwidth_iw6);

		auto reply = server->create_message(5);
		reply.send(&buffer, true);
	}
}
