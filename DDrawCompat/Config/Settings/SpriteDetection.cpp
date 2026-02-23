#include <Config/Settings/SpriteDetection.h>

namespace Config
{
	namespace Settings
	{
		SpriteDetection::SpriteDetection()
			: EnumSetting("SpriteDetection", "off", { "off",  "zconst", "zmax", "point" })
		{
		}

		Setting::ParamInfo SpriteDetection::getParamInfo() const
		{
			if (ZMAX == m_value)
			{
				return { "ZMax", 0, 100, 0 };
			}
			if (POINT == m_value)
			{
				return { "ZConst", 0, 1, 1 };
			}
			return {};
		}
	}
}
