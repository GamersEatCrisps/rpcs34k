#include "stdafx.h"
#include "sys_gamepad.h"

LOG_CHANNEL(sys_gamepad);

u32 sys_gamepad_ycon_initalize(vm::ptr<uint8_t> in, vm::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_initalize(in=%d, out=%d) -> CELL_OK", in, out);
	return CELL_OK;
}

u32 sys_gamepad_ycon_finalize(vm::ptr<uint8_t> in, vm::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_finalize(in=%d, out=%d) -> CELL_OK", in, out);
	return CELL_OK;
}

u32 sys_gamepad_ycon_has_input_ownership(vm::ptr<uint8_t> in, vm::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_has_input_ownership(in=%d, out=%d) -> CELL_OK", in, out);
	return CELL_OK;
}

u32 sys_gamepad_ycon_enumerate_device(vm::ptr<uint8_t> in, vm::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_enumerate_device(in=%d, out=%d) -> CELL_OK", in, out);
	return CELL_OK;
}

u32 sys_gamepad_ycon_get_device_info(vm::ptr<uint8_t> in, vm::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_get_device_info(in=%d, out=%d) -> CELL_OK", in, out);
	return CELL_OK;
}

u32 sys_gamepad_ycon_read_raw_report(vm::ptr<uint8_t> in, vm::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_read_raw_report(in=%d, out=%d) -> CELL_OK", in, out);
	return CELL_OK;
}

u32 sys_gamepad_ycon_write_raw_report(vm::ptr<uint8_t> in, vm::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_write_raw_report(in=%d, out=%d) -> CELL_OK", in, out);
	return CELL_OK;
}

u32 sys_gamepad_ycon_get_feature(vm::ptr<uint8_t> in, vm::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_get_feature(in=%d, out=%d) -> CELL_OK", in, out);
	return CELL_OK;
}

u32 sys_gamepad_ycon_set_feature(vm::ptr<uint8_t> in, vm::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_set_feature(in=%d, out=%d) -> CELL_OK", in, out);
	return CELL_OK;
}

u32 sys_gamepad_ycon_is_gem(vm::ptr<uint8_t> in, vm::ptr<uint8_t> out)
{
	sys_gamepad.todo("sys_gamepad_ycon_is_gem(in=%d, out=%d) -> CELL_OK", in, out);
	return CELL_OK;
}

// syscall(621,packet_id,uint8_t *in,uint8_t *out) Talk:LV2_Functions_and_Syscalls#Syscall_621_.280x26D.29 gamepad_if usage
u32 sys_gamepad_ycon_if(uint8_t packet_id, vm::ptr<uint8_t> in, vm::ptr<uint8_t> out)
{

	switch (packet_id)
	{
	case 0:
		return sys_gamepad_ycon_initalize(in, out);
		break;
	case 1:
		return sys_gamepad_ycon_finalize(in, out);
		break;
	case 2:
		return sys_gamepad_ycon_has_input_ownership(in, out);
		break;
	case 3:
		return sys_gamepad_ycon_enumerate_device(in, out);
		break;
	case 4:
		return sys_gamepad_ycon_get_device_info(in, out);
		break;
	case 5:
		return sys_gamepad_ycon_read_raw_report(in, out);
		break;
	case 6:
		return sys_gamepad_ycon_write_raw_report(in, out);
		break;
	case 7:
		return sys_gamepad_ycon_get_feature(in, out);
		break;
	case 8:
		return sys_gamepad_ycon_set_feature(in, out);
		break;
	case 9:
		return sys_gamepad_ycon_is_gem(in, out);
		break;

	default:
		sys_gamepad.error("sys_gamepad_ycon_if(packet_id=*%d, in=%d, out=%d), unknown packet id", packet_id, in, out);
		break;
	}

	return CELL_OK;
}
