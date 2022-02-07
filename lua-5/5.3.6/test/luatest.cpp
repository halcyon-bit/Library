#include <iostream>
#include "lua.hpp"

static const luaL_Reg lua_reg_libs[] = {
    { "base", luaopen_base},
//	{ "math", luaopen_math},
    { NULL, NULL },
};

int main(int argc, char* argv[])
{
    if (lua_State* L = luaL_newstate())
	{

        const luaL_Reg* lua_reg = lua_reg_libs;
        for (; lua_reg->func; ++lua_reg)
 		{
            luaL_requiref(L, lua_reg->name, lua_reg->func, 1);
            lua_pop(L, 1);
        }
		
        //加载脚本，如果出错，则打印错误
		if (luaL_dofile(L, "lua.lua"))
//        if (luaL_dostring(L, "a"))
		{
            std::cout << lua_tostring(L, -1) << std::endl;
        }
		if (!lua_isstring(L, -1))
		{
			int a = lua_tointeger(L, -1);
			std::cout << a << std::endl;
		}
//		std::cout << lua_tostring(L, -1) << std::endl;
        lua_close(L);
    }
    else
	{
        std::cout << "luaL_newstate error !" << std::endl;
    }
	
    system("pause");
    return 0;
}