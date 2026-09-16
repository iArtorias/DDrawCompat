#pragma once

#include <Config/EnumSetting.h>

namespace Config
{
	namespace Settings
	{
		class PresentDelay : public EnumSetting
		{
		public:
			enum Values { OFF, ON, AUTO };

			PresentDelay() : EnumSetting("PresentDelay", "auto", { "off", "on", "auto" })
			{
			}

			virtual ParamInfo getParamInfo() const override
			{
				if (OFF != m_value)
				{
					return { "MaxDelay", 1, 50, 10 };
				}
				return {};
			}
		};
	}

	extern Settings::PresentDelay presentDelay;
}
