#include <algorithm>
#include <set>

#include <intrin.h>

#include <Common/CompatPtr.h>
#include <Common/Log.h>
#include <D3dDdi/FormatInfo.h>
#include <D3dDdi/Resource.h>
#include <D3dDdi/SurfaceRepository.h>
#include <DDraw/DirectDraw.h>
#include <DDraw/DirectDrawSurface.h>
#include <DDraw/RealPrimarySurface.h>
#include <DDraw/ScopedThreadLock.h>
#include <DDraw/Surfaces/OverlaySurface.h>
#include <DDraw/Surfaces/OverlaySurfaceImpl.h>
#include <DDraw/Surfaces/PrimarySurface.h>
#include <DDraw/Surfaces/TagSurface.h>

namespace
{
	const DWORD FOURCC_UYVY = MAKEFOURCC( 'U', 'Y', 'V', 'Y' );
	const DWORD FOURCC_YUY2 = MAKEFOURCC( 'Y', 'U', 'Y', '2' );

	std::set<DDraw::OverlaySurface *> g_overlays;
	bool g_isEmulationEnabled = false;
	D3dDdi::SurfaceRepository::Surface * g_composedSurface = nullptr;
	D3dDdi::SurfaceRepository * g_composedSurfaceRepo = nullptr;

	struct ColorChannel
	{
		DWORD mask = 0;
		DWORD shift = 0;
		DWORD max = 0;

		ColorChannel( DWORD mask ) : mask( mask )
		{
			unsigned long index = 0;
			if (0 != mask && _BitScanForward( &index, mask ))
			{
				shift = index;
				max = mask >> shift;
			}
		}

		BYTE read( DWORD pixel ) const
		{
			return 0 != max ? static_cast<BYTE>(((pixel & mask) >> shift) * 255 / max) : 0;
		}

		DWORD write( BYTE value ) const
		{
			return 0 != max ? ((value * max + 127) / 255) << shift : 0;
		}
	};

	struct RgbFormat
	{
		ColorChannel red;
		ColorChannel green;
		ColorChannel blue;
		DWORD bytesPerPixel;

		RgbFormat( const DDPIXELFORMAT & pf )
			: red( pf.dwRBitMask )
			, green( pf.dwGBitMask )
			, blue( pf.dwBBitMask )
			, bytesPerPixel( pf.dwRGBBitCount / 8 )
		{}
	};

	BYTE clampToByte( int value )
	{
		return static_cast<BYTE>(std::clamp( value, 0, 255 ));
	}

	DWORD yuvToRgb( BYTE y, BYTE u, BYTE v )
	{
		const int c = y - 16;
		const int d = u - 128;
		const int e = v - 128;
		const BYTE r = clampToByte( (298 * c + 409 * e + 128) >> 8 );
		const BYTE g = clampToByte( (298 * c - 100 * d - 208 * e + 128) >> 8 );
		const BYTE b = clampToByte( (298 * c + 516 * d + 128) >> 8 );
		return (r << 16) | (g << 8) | b;
	}

	bool isRgbFormat( const DDPIXELFORMAT & pf )
	{
		return (pf.dwFlags & DDPF_RGB) &&
			!(pf.dwFlags & (DDPF_PALETTEINDEXED1 | DDPF_PALETTEINDEXED2 | DDPF_PALETTEINDEXED4 | DDPF_PALETTEINDEXED8)) &&
			(16 == pf.dwRGBBitCount || 24 == pf.dwRGBBitCount || 32 == pf.dwRGBBitCount);
	}

	bool isSupportedFourCC( DWORD fourCC )
	{
		return FOURCC_YUY2 == fourCC || FOURCC_UYVY == fourCC;
	}

	DWORD readPixel( const BYTE * p, DWORD bytesPerPixel )
	{
		switch (bytesPerPixel)
		{
			case 2:
				return *reinterpret_cast<const WORD *>(p);
			case 3:
				return p[0] | (p[1] << 8) | (p[2] << 16);
			default:
				return *reinterpret_cast<const DWORD *>(p);
		}
	}

	void writePixel( BYTE * p, DWORD bytesPerPixel, DWORD pixel )
	{
		switch (bytesPerPixel)
		{
			case 2:
				*reinterpret_cast<WORD *>(p) = static_cast<WORD>(pixel);
				break;
			case 3:
				p[0] = static_cast<BYTE>(pixel);
				p[1] = static_cast<BYTE>(pixel >> 8);
				p[2] = static_cast<BYTE>(pixel >> 16);
				break;
			default:
				*reinterpret_cast<DWORD *>(p) = pixel;
				break;
		}
	}

	RECT getSurfaceRect( CompatWeakPtr<IDirectDrawSurface7> surface )
	{
		DDSURFACEDESC2 desc = {};
		desc.dwSize = sizeof( desc );
		if (FAILED( surface->GetSurfaceDesc( surface, &desc ) ))
		{
			return {};
		}
		return { 0, 0, static_cast<LONG>(desc.dwWidth), static_cast<LONG>(desc.dwHeight) };
	}
}

namespace DDraw
{
	OverlaySurface::OverlaySurface( DWORD origFlags, const Caps & origCaps )
		: Surface( origFlags, origCaps )
	{
		g_overlays.insert( this );
	}

	OverlaySurface::~OverlaySurface()
	{
		g_overlays.erase( this );
		if (m_isVisible)
		{
			RealPrimarySurface::scheduleUpdate();
		}
	}

	void OverlaySurface::addCaps( DDCAPS & caps )
	{
		caps.dwCaps |= DDCAPS_OVERLAY | DDCAPS_OVERLAYFOURCC | DDCAPS_OVERLAYSTRETCH | DDCAPS_COLORKEY;
		caps.dwCKeyCaps |= DDCKEYCAPS_DESTOVERLAY | DDCKEYCAPS_DESTOVERLAYONEACTIVE;
		caps.dwFXCaps |= DDFXCAPS_OVERLAYSHRINKX | DDFXCAPS_OVERLAYSHRINKY |
			DDFXCAPS_OVERLAYSTRETCHX | DDFXCAPS_OVERLAYSTRETCHY;
		caps.ddsCaps.dwCaps |= DDSCAPS_OVERLAY;
		caps.dwMaxVisibleOverlays = 1;
		caps.dwCurrVisibleOverlays = static_cast<DWORD>(std::count_if( g_overlays.begin(), g_overlays.end(),
			[] ( const OverlaySurface * overlay )
		{
			return overlay->m_isVisible;
		} ));
		caps.dwMinOverlayStretch = 1;
		caps.dwMaxOverlayStretch = 32000;
	}

	void OverlaySurface::addFourCCCodes( std::vector<DWORD> & codes )
	{
		for (DWORD fourCC : { FOURCC_YUY2, FOURCC_UYVY })
		{
			if (std::find( codes.begin(), codes.end(), fourCC ) == codes.end())
			{
				codes.push_back( fourCC );
			}
		}
	}

	void OverlaySurface::blendTo( const DDSURFACEDESC2 & dstDesc )
	{
		RECT destRect = {};
		const RECT dstBounds = { 0, 0, static_cast<LONG>(dstDesc.dwWidth), static_cast<LONG>(dstDesc.dwHeight) };
		const LONG srcWidth = m_srcRect.right - m_srcRect.left;
		const LONG srcHeight = m_srcRect.bottom - m_srcRect.top;
		const LONG destWidth = m_destRect.right - m_destRect.left;
		const LONG destHeight = m_destRect.bottom - m_destRect.top;
		if (srcWidth <= 0 || srcHeight <= 0 || destWidth <= 0 || destHeight <= 0 ||
			!IntersectRect( &destRect, &m_destRect, &dstBounds ))
		{
			return;
		}

		DDSURFACEDESC2 srcDesc = {};
		srcDesc.dwSize = sizeof( srcDesc );
		if (FAILED( m_surface->Lock( m_surface, nullptr, &srcDesc, DDLOCK_WAIT | DDLOCK_READONLY, nullptr ) ))
		{
			return;
		}

		const bool isFourCC = 0 != (m_pixelFormat.dwFlags & DDPF_FOURCC);
		const bool isUyvy = isFourCC && FOURCC_UYVY == m_pixelFormat.dwFourCC;
		const RgbFormat srcFormat( m_pixelFormat );
		const RgbFormat dstFormat( dstDesc.ddpfPixelFormat );
		const DWORD keyMask = dstFormat.bytesPerPixel >= 4 ? 0xFFFFFFFF : (1 << (dstFormat.bytesPerPixel * 8)) - 1;
		const DWORD keyLow = m_destColorKey.dwColorSpaceLowValue & keyMask;
		const DWORD keyHigh = std::max( m_destColorKey.dwColorSpaceHighValue & keyMask, keyLow );

		for (LONG y = destRect.top; y < destRect.bottom; ++y)
		{
			const LONG srcY = m_srcRect.top + (y - m_destRect.top) * srcHeight / destHeight;
			const BYTE * srcRow = static_cast<const BYTE *>( srcDesc.lpSurface ) + srcY * srcDesc.lPitch;
			BYTE * dstRow = static_cast<BYTE *>( dstDesc.lpSurface ) + y * dstDesc.lPitch;

			for (LONG x = destRect.left; x < destRect.right; ++x)
			{
				BYTE * dst = dstRow + x * dstFormat.bytesPerPixel;
				if (m_isDestColorKeyEnabled)
				{
					const DWORD dstPixel = readPixel( dst, dstFormat.bytesPerPixel );
					if (dstPixel < keyLow || dstPixel > keyHigh)
					{
						continue;
					}
				}

				const LONG srcX = m_srcRect.left + (x - m_destRect.left) * srcWidth / destWidth;
				DWORD rgb = 0;
				if (isFourCC)
				{
					const BYTE * macroPixel = srcRow + (srcX / 2) * 4;
					if (isUyvy)
					{
						rgb = yuvToRgb( macroPixel[1 + (srcX & 1) * 2], macroPixel[0], macroPixel[2] );
					}
					else
					{
						rgb = yuvToRgb( macroPixel[(srcX & 1) * 2], macroPixel[1], macroPixel[3] );
					}
				}
				else
				{
					const DWORD srcPixel = readPixel( srcRow + srcX * srcFormat.bytesPerPixel, srcFormat.bytesPerPixel );
					rgb = (srcFormat.red.read( srcPixel ) << 16) |
						(srcFormat.green.read( srcPixel ) << 8) |
						srcFormat.blue.read( srcPixel );
				}

				writePixel( dst, dstFormat.bytesPerPixel,
					dstFormat.red.write( static_cast<BYTE>(rgb >> 16) ) |
					dstFormat.green.write( static_cast<BYTE>(rgb >> 8) ) |
					dstFormat.blue.write( static_cast<BYTE>(rgb) ) );
			}
		}

		m_surface->Unlock( m_surface, nullptr );
	}

	CompatWeakPtr<IDirectDrawSurface7> OverlaySurface::compose( PrimarySurface & primary )
	{
		std::vector<OverlaySurface *> overlays;
		for (auto overlay : g_overlays)
		{
			if (overlay->m_isVisible)
			{
				overlays.push_back( overlay );
			}
		}

		if (overlays.empty())
		{
			return nullptr;
		}

		LOG_FUNC( "OverlaySurface::compose", &primary );
		auto primaryDds( primary.getDDS() );
		DDSURFACEDESC2 desc = {};
		desc.dwSize = sizeof( desc );
		if (FAILED( primaryDds->GetSurfaceDesc( primaryDds, &desc ) ))
		{
			return nullptr;
		}

		if (!isRgbFormat( desc.ddpfPixelFormat ))
		{
			LOG_ONCE( "Overlay emulation is not supported in palettized display modes" );
			return nullptr;
		}

		auto repo = DirectDrawSurface::getSurfaceRepository( *primaryDds );
		if (!repo)
		{
			return nullptr;
		}

		if (!g_composedSurface)
		{
			g_composedSurface = new D3dDdi::SurfaceRepository::Surface();
		}
		if (g_composedSurfaceRepo != repo)
		{
			*g_composedSurface = {};
			g_composedSurfaceRepo = repo;
		}

		auto & composed = repo->getSurface( *g_composedSurface, desc.dwWidth, desc.dwHeight,
			D3dDdi::getFormat( desc.ddpfPixelFormat ), DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY );
		if (!composed.surface)
		{
			return nullptr;
		}

		DDSURFACEDESC2 dstDesc = {};
		dstDesc.dwSize = sizeof( dstDesc );
		if (FAILED( composed.surface->Lock( composed.surface, nullptr, &dstDesc, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr ) ))
		{
			return nullptr;
		}

		DDSURFACEDESC2 srcDesc = {};
		srcDesc.dwSize = sizeof( srcDesc );
		D3dDdi::Resource::setReadOnlyLock( true );
		HRESULT result = primaryDds->Lock( primaryDds, nullptr, &srcDesc, DDLOCK_WAIT | DDLOCK_READONLY, nullptr );
		D3dDdi::Resource::setReadOnlyLock( false );
		if (FAILED( result ))
		{
			composed.surface->Unlock( composed.surface, nullptr );
			return nullptr;
		}

		const DWORD rowSize = desc.dwWidth * desc.ddpfPixelFormat.dwRGBBitCount / 8;
		for (DWORD y = 0; y < desc.dwHeight; ++y)
		{
			memcpy( static_cast<BYTE *>( dstDesc.lpSurface ) + y * dstDesc.lPitch,
				static_cast<const BYTE *>( srcDesc.lpSurface ) + y * srcDesc.lPitch, rowSize );
		}
		primaryDds->Unlock( primaryDds, nullptr );

		dstDesc.ddpfPixelFormat = desc.ddpfPixelFormat;
		for (auto overlay : overlays)
		{
			overlay->blendTo( dstDesc );
		}

		composed.surface->Unlock( composed.surface, nullptr );
		return composed.surface.get();
	}

	template <typename TDirectDraw, typename TSurface, typename TSurfaceDesc>
	HRESULT OverlaySurface::create( CompatRef<TDirectDraw> dd, TSurfaceDesc desc, TSurface *& surface )
	{
		LOG_FUNC( "OverlaySurface::create", &dd, desc, surface );

		DWORD backBufferCount = 0;
		if ((desc.ddsCaps.dwCaps & DDSCAPS_FLIP) && (desc.dwFlags & DDSD_BACKBUFFERCOUNT))
		{
			backBufferCount = desc.dwBackBufferCount;
		}
		desc.dwFlags &= ~DDSD_BACKBUFFERCOUNT;
		desc.dwBackBufferCount = 0;

		DDPIXELFORMAT pf = {};
		if (desc.dwFlags & DDSD_PIXELFORMAT)
		{
			pf = desc.ddpfPixelFormat;
			if (pf.dwFlags & DDPF_FOURCC)
			{
				if (!isSupportedFourCC( pf.dwFourCC ))
				{
					return LOG_RESULT( DDERR_INVALIDPIXELFORMAT );
				}
				desc.ddpfPixelFormat = DirectDraw::getRgbPixelFormat( 16 );
			}
			else if (!isRgbFormat( pf ))
			{
				return LOG_RESULT( DDERR_INVALIDPIXELFORMAT );
			}
		}

		desc.ddsCaps.dwCaps &= ~(DDSCAPS_OVERLAY | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM |
			DDSCAPS_NONLOCALVIDMEM | DDSCAPS_VISIBLE | DDSCAPS_HWCODEC |
			DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER);
		desc.ddsCaps.dwCaps |= DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;

		auto privateData( std::make_unique<OverlaySurface>( desc.dwFlags, desc.ddsCaps ) );
		auto overlay = privateData.get();
		HRESULT result = Surface::create( dd, desc, surface, std::move( privateData ) );
		if (FAILED( result ))
		{
			return LOG_RESULT( result );
		}

		if (!(pf.dwFlags & DDPF_FOURCC))
		{
			TSurfaceDesc actualDesc = {};
			actualDesc.dwSize = sizeof( actualDesc );
			getOrigVtable( surface ).GetSurfaceDesc( surface, &actualDesc );
			pf = actualDesc.ddpfPixelFormat;
			if (!isRgbFormat( pf ))
			{
				getOrigVtable( surface ).Release( surface );
				surface = nullptr;
				return LOG_RESULT( DDERR_INVALIDPIXELFORMAT );
			}
		}

		overlay->m_pixelFormat = pf;
		overlay->m_backBufferCount = backBufferCount;
		LOG_ONCE( "Using feature: overlay surface emulation" );
		return LOG_RESULT( DD_OK );
	}

	template HRESULT OverlaySurface::create(
		CompatRef<IDirectDraw> dd, DDSURFACEDESC desc, IDirectDrawSurface *& surface );
	template HRESULT OverlaySurface::create(
		CompatRef<IDirectDraw2> dd, DDSURFACEDESC desc, IDirectDrawSurface *& surface );
	template HRESULT OverlaySurface::create(
		CompatRef<IDirectDraw4> dd, DDSURFACEDESC2 desc, IDirectDrawSurface4 *& surface );
	template HRESULT OverlaySurface::create(
		CompatRef<IDirectDraw7> dd, DDSURFACEDESC2 desc, IDirectDrawSurface7 *& surface );

	void OverlaySurface::createImpl()
	{
		m_impl.reset( new OverlaySurfaceImpl<IDirectDrawSurface>( this ) );
		m_impl2.reset( new OverlaySurfaceImpl<IDirectDrawSurface2>( this ) );
		m_impl3.reset( new OverlaySurfaceImpl<IDirectDrawSurface3>( this ) );
		m_impl4.reset( new OverlaySurfaceImpl<IDirectDrawSurface4>( this ) );
		m_impl7.reset( new OverlaySurfaceImpl<IDirectDrawSurface7>( this ) );
	}

	void OverlaySurface::enableEmulation( IUnknown * dd )
	{
		LOG_FUNC( "OverlaySurface::enableEmulation", dd );
		if (!dd)
		{
			return;
		}

		CompatPtr<IDirectDraw> dd1;
		if (FAILED( dd->lpVtbl->QueryInterface( dd, IID_IDirectDraw, reinterpret_cast<void **>(&dd1.getRef()) ) ) || !dd1)
		{
			return;
		}

		DDraw::ScopedThreadLock lock;
		auto tagSurface = TagSurface::get( *dd1 );
		if (tagSurface && !tagSurface->isOverlayEmulated())
		{
			tagSurface->setOverlayEmulated( true );
			g_isEmulationEnabled = true;
			LOG_ONCE( "Using feature: overlay emulation for DirectShow exclusive mode video" );
		}
	}

	HRESULT OverlaySurface::getPosition( LONG & x, LONG & y ) const
	{
		if (!m_isVisible)
		{
			return DDERR_OVERLAYNOTVISIBLE;
		}
		x = m_destRect.left;
		y = m_destRect.top;
		return DD_OK;
	}

	bool OverlaySurface::isEmulationEnabled()
	{
		return g_isEmulationEnabled;
	}

	void OverlaySurface::onContentChanged()
	{
		if (m_isVisible)
		{
			RealPrimarySurface::scheduleUpdate();
		}
	}

	HRESULT OverlaySurface::setPosition( LONG x, LONG y )
	{
		if (!m_isVisible)
		{
			return DDERR_OVERLAYNOTVISIBLE;
		}
		OffsetRect( &m_destRect, x - m_destRect.left, y - m_destRect.top );
		RealPrimarySurface::scheduleUpdate();
		return DD_OK;
	}

	HRESULT OverlaySurface::update(
		const RECT * srcRect, Surface * destSurface, const RECT * destRect, DWORD flags, const DDOVERLAYFX * fx )
	{
		if (flags & DDOVER_HIDE)
		{
			if (m_isVisible)
			{
				m_isVisible = false;
				RealPrimarySurface::scheduleUpdate();
			}
			return DD_OK;
		}

		if (!destSurface || !destSurface->isPrimary())
		{
			return DDERR_INVALIDPARAMS;
		}

		const RECT srcBounds = getSurfaceRect( m_surface );
		if (srcRect)
		{
			if (!IntersectRect( &m_srcRect, srcRect, &srcBounds ))
			{
				return DDERR_INVALIDRECT;
			}
		}
		else
		{
			m_srcRect = srcBounds;
		}

		if (destRect)
		{
			m_destRect = *destRect;
		}
		else if (IsRectEmpty( &m_destRect ))
		{
			m_destRect = getSurfaceRect( destSurface->getDDS() );
		}

		m_isDestColorKeyEnabled = false;
		if ((flags & DDOVER_KEYDESTOVERRIDE) && fx)
		{
			m_isDestColorKeyEnabled = true;
			m_destColorKey = fx->dckDestColorkey;
		}
		else if (flags & DDOVER_KEYDEST)
		{
			auto primary = dynamic_cast<PrimarySurface *>(destSurface);
			auto colorKey = primary ? primary->getDestOverlayColorKey() : nullptr;
			if (colorKey)
			{
				m_isDestColorKeyEnabled = true;
				m_destColorKey = *colorKey;
			}
			else
			{
				auto destDds( destSurface->getDDS() );
				m_isDestColorKeyEnabled = SUCCEEDED( destDds->GetColorKey( destDds, DDCKEY_DESTOVERLAY, &m_destColorKey ) );
			}
		}

		if (flags & (DDOVER_KEYSRC | DDOVER_KEYSRCOVERRIDE))
		{
			LOG_ONCE( "Overlay emulation: source color keys are not supported" );
		}

		if (flags & DDOVER_SHOW)
		{
			m_isVisible = true;
		}

		RealPrimarySurface::scheduleUpdate();
		return DD_OK;
	}
}