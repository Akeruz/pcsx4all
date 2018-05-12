/***************************************************************************
*   Copyright (C) 2010 PCSX4ALL Team                                      *
*   Copyright (C) 2010 Unai                                               *
*   Copyright (C) 2011 notaz                                              *
*   Copyright (C) 2016 Senquack (dansilsby <AT> gmail <DOT> com)          *
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
*   This program is distributed in the hope that it will be useful,       *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
*   GNU General Public License for more details.                          *
*                                                                         *
*   You should have received a copy of the GNU General Public License     *
*   along with this program; if not, write to the                         *
*   Free Software Foundation, Inc.,                                       *
*   51 Franklin Street, Fifth Floor, Boston, MA 02111-1307 USA.           *
***************************************************************************/

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/gpulib/gpu.h"
#include "port.h"
#include "gpu_unai.h"

#define GPU_INLINE static inline __attribute__((always_inline))

// GPU fixed point math
#include "gpu_fixedpoint.h"

// Inner loop driver instantiation file
#include "gpu_inner.h"

// GPU internal image drawing functions
#include "gpu_raster_image.h"

// GPU internal line drawing functions
#include "gpu_raster_line.h"

// GPU internal polygon drawing functions
#include "gpu_raster_polygon.h"

// GPU internal sprite drawing functions
#include "gpu_raster_sprite.h"

// GPU command buffer execution/store
#include "gpu_command.h"

/////////////////////////////////////////////////////////////////////////////

int renderer_init(void)
{
  memset((void*)&gpu_unai, 0, sizeof(gpu_unai));
  gpu_unai.vram = (u16*)gpu.vram;

  // Original standalone gpu_unai initialized TextureWindow[]. I added the
  //  same behavior here, since it seems unsafe to leave [2],[3] unset when
  //  using HLE and Rearmed gpu_neon sets this similarly on init. -senquack
  gpu_unai.TextureWindow[0] = 0;
  gpu_unai.TextureWindow[1] = 0;
  gpu_unai.TextureWindow[2] = 255;
  gpu_unai.TextureWindow[3] = 255;
  //senquack - new vars must be updated whenever texture window is changed:
  //           (used for polygon-drawing in gpu_inner.h, gpu_raster_polygon.h)
  const u32 fb = FIXED_BITS;  // # of fractional fixed-pt bits of u4/v4
  gpu_unai.u_msk = (((u32)gpu_unai.TextureWindow[2]) << fb) | ((1 << fb) - 1);
  gpu_unai.v_msk = (((u32)gpu_unai.TextureWindow[3]) << fb) | ((1 << fb) - 1);

  // Configuration options
  gpu_unai.config = gpu_unai_config_ext;
  //senquack - disabled, not sure this is needed and would require modifying
  // sprite-span functions, perhaps unnecessarily. No Abe Oddysey hack was
  // present in latest PCSX4ALL sources we were using.
  //gpu_unai.config.enableAbbeyHack = gpu_unai_config_ext.abe_hack;
  gpu_unai.ilace_mask = gpu_unai.config.ilace_force;

#ifdef GPU_UNAI_USE_INT_DIV_MULTINV
  // s_invTable
  for(int i=1; i<=(1<<TABLE_BITS); ++i)
  {
    double v = 1.0 / double(i);
#ifdef GPU_TABLE_10_BITS
    v *= double(0xffffffff>>1);
#else
    v *= double(0x80000000);
#endif
    s_invTable[i-1]=s32(v);
  }
#endif

  SetupLightLUT();
  SetupDitheringConstants();

  return 0;
}

void renderer_finish(void)
{
}

void renderer_notify_res_change(void)
{
  /*
   if (PixelSkipEnabled()) {
     // Set blit_mask for high horizontal resolutions. This allows skipping
     //  rendering pixels that would never get displayed on low-resolution
     //  platforms that use simple pixel-dropping scaler.

     switch (gpu.screen.hres)
     {
       case 512: gpu_unai.blit_mask = 0xa4; break; // GPU_BlitWWSWWSWS
       case 640: gpu_unai.blit_mask = 0xaa; break; // GPU_BlitWS
       default:  gpu_unai.blit_mask = 0;    break;
     }
   } else */
  {
    gpu_unai.blit_mask = 0;
  }

  if (LineSkipEnabled())
  {
    // Set rendering line-skip (only render every other line in high-res
    //  480 vertical mode, or, optionally, force it for all video modes)

    if (gpu.screen.vres == 480)
    {
      if (gpu_unai.config.ilace_force)
      {
        gpu_unai.ilace_mask = 3; // Only need 1/4 of lines
      }
      else
      {
        gpu_unai.ilace_mask = 1; // Only need 1/2 of lines
      }
    }
    else
    {
      // Vert resolution changed from 480 to lower one
      gpu_unai.ilace_mask = gpu_unai.config.ilace_force;
    }
  }
  else
  {
    gpu_unai.ilace_mask = 0;
  }

  printf("res change hres: %d   vres: %d   depth: %d   ilace_mask: %d\n",
         gpu.screen.hres, gpu.screen.vres, gpu.status.rgb24 ? 24 : 15,
         gpu_unai.ilace_mask);
}

// Handles GP0 draw settings commands 0xE1...0xE6
static void gpuGP0Cmd_0xEx(gpu_unai_t &gpu_unai, u32 cmd_word)
{
  // Assume incoming GP0 command is 0xE1..0xE6, convert to 1..6
  u8 num = (cmd_word >> 24) & 7;
  gpu.ex_regs[num] = cmd_word; // Update gpulib register
  switch (num)
  {
    case 1:
    {
      // GP0(E1h) - Draw Mode setting (aka "Texpage")
      u32 cur_texpage = gpu_unai.GPU_GP1 & 0x7FF;
      u32 new_texpage = cmd_word & 0x7FF;
      if (cur_texpage != new_texpage)
      {
        gpu_unai.GPU_GP1 = (gpu_unai.GPU_GP1 & ~0x7FF) | new_texpage;
        gpuSetTexture(gpu_unai.GPU_GP1);
      }
    }
    break;

    case 2:
    {
      // GP0(E2h) - Texture Window setting
      if (cmd_word != gpu_unai.TextureWindowCur)
      {
        static const u8 TextureMask[32] =
        {
          255, 7, 15, 7, 31, 7, 15, 7, 63, 7, 15, 7, 31, 7, 15, 7,
          127, 7, 15, 7, 31, 7, 15, 7, 63, 7, 15, 7, 31, 7, 15, 7
        };
        gpu_unai.TextureWindowCur = cmd_word;
        gpu_unai.TextureWindow[0] = ((cmd_word >> 10) & 0x1F) << 3;
        gpu_unai.TextureWindow[1] = ((cmd_word >> 15) & 0x1F) << 3;
        gpu_unai.TextureWindow[2] = TextureMask[(cmd_word >> 0) & 0x1F];
        gpu_unai.TextureWindow[3] = TextureMask[(cmd_word >> 5) & 0x1F];
        gpu_unai.TextureWindow[0] &= ~gpu_unai.TextureWindow[2];
        gpu_unai.TextureWindow[1] &= ~gpu_unai.TextureWindow[3];

        // Inner loop vars must be updated whenever texture window is changed:
        const u32 fb = FIXED_BITS;  // # of fractional fixed-pt bits of u4/v4
        gpu_unai.u_msk = (((u32)gpu_unai.TextureWindow[2]) << fb) | ((1 << fb) - 1);
        gpu_unai.v_msk = (((u32)gpu_unai.TextureWindow[3]) << fb) | ((1 << fb) - 1);

        gpuSetTexture(gpu_unai.GPU_GP1);
      }
    }
    break;

    case 3:
    {
      // GP0(E3h) - Set Drawing Area top left (X1,Y1)
      gpu_unai.DrawingArea[0] = cmd_word         & 0x3FF;
      gpu_unai.DrawingArea[1] = (cmd_word >> 10) & 0x3FF;
    }
    break;

    case 4:
    {
      // GP0(E4h) - Set Drawing Area bottom right (X2,Y2)
      gpu_unai.DrawingArea[2] = (cmd_word         & 0x3FF) + 1;
      gpu_unai.DrawingArea[3] = ((cmd_word >> 10) & 0x3FF) + 1;
    }
    break;

    case 5:
    {
      // GP0(E5h) - Set Drawing Offset (X,Y)
      gpu_unai.DrawingOffset[0] = ((s32)cmd_word<<(32-11))>>(32-11);
      gpu_unai.DrawingOffset[1] = ((s32)cmd_word<<(32-22))>>(32-11);
    }
    break;

    case 6:
    {
      // GP0(E6h) - Mask Bit Setting
      gpu_unai.Masking  = (cmd_word & 0x2) <<  1;
      gpu_unai.PixelMSB = (cmd_word & 0x1) <<  8;
    }
    break;
  }
}

extern const unsigned char cmd_lengths[256];

int do_cmd_list(unsigned int *list, int list_len, int *last_cmd)
{
  unsigned int cmd = 0, len, i;
  unsigned int *list_start = list;
  unsigned int *list_end = list + list_len;

  // TODO: set ilace_mask when resolution changes instead of every time,
  // eliminate #ifdef below.
  gpu_unai.ilace_mask = gpu_unai.config.ilace_force;

#ifdef HAVE_PRE_ARMV7 /* XXX */
  gpu_unai.ilace_mask |= gpu.status.interlace;
#endif

  for (; list < list_end; list += 1 + len)
  {
    cmd = *list >> 24;
    len = cmd_lengths[cmd];
    if (list + 1 + len > list_end)
    {
      cmd = -1;
      break;
    }

#define PRIM cmd
    gpu_unai.PacketBuffer.U4[0] = list[0];
    for (i = 1; i <= len; i++)
      gpu_unai.PacketBuffer.U4[i] = list[i];

    PtrUnion packet;
    packet.ptr = (void*)&gpu_unai.PacketBuffer;

    switch (cmd)
    {
      case 0x02:
        gpuClearImage(packet);
        break;

      case 0x20:
      case 0x21:
      case 0x22:
      case 0x23:            // Monochrome 3-pt poly
      {
        PP driver = gpuPolySpanDrivers[
             (gpu_unai.blit_mask?1024:0) |
             Blending_Mode |
             gpu_unai.Masking | Blending | gpu_unai.PixelMSB
           ];
        gpuDrawPolyF(packet, driver, false);
      }
      break;

      case 0x24:
      case 0x25:
      case 0x26:
      case 0x27:            // Textured 3-pt poly
      {
        gpuSetCLUT   (gpu_unai.PacketBuffer.U4[2] >> 16);
        gpuSetTexture(gpu_unai.PacketBuffer.U4[4] >> 16);

        u32 driver_idx =
          (gpu_unai.blit_mask?1024:0) |
          Dithering |
          Blending_Mode | gpu_unai.TEXT_MODE |
          gpu_unai.Masking | Blending | gpu_unai.PixelMSB;

        if (!FastLightingEnabled())
        {
          driver_idx |= Lighting;
        }
        else
        {
          if (!((gpu_unai.PacketBuffer.U1[0]>0x5F) && (gpu_unai.PacketBuffer.U1[1]>0x5F) && (gpu_unai.PacketBuffer.U1[2]>0x5F)))
            driver_idx |= Lighting;
        }

        PP driver = gpuPolySpanDrivers[driver_idx];
        gpuDrawPolyFT(packet, driver, false);
      }
      break;

      case 0x28:
      case 0x29:
      case 0x2A:
      case 0x2B:            // Monochrome 4-pt poly
      {
        PP driver = gpuPolySpanDrivers[
             (gpu_unai.blit_mask?1024:0) |
             Blending_Mode |
             gpu_unai.Masking | Blending | gpu_unai.PixelMSB
           ];
        gpuDrawPolyF(packet, driver, true); // is_quad = true
      }
      break;

      case 0x2C:
      case 0x2D:
      case 0x2E:
      case 0x2F:            // Textured 4-pt poly
      {
        gpuSetCLUT   (gpu_unai.PacketBuffer.U4[2] >> 16);
        gpuSetTexture(gpu_unai.PacketBuffer.U4[4] >> 16);

        u32 driver_idx =
          (gpu_unai.blit_mask?1024:0) |
          Dithering |
          Blending_Mode | gpu_unai.TEXT_MODE |
          gpu_unai.Masking | Blending | gpu_unai.PixelMSB;

        if (!FastLightingEnabled())
        {
          driver_idx |= Lighting;
        }
        else
        {
          if (!((gpu_unai.PacketBuffer.U1[0]>0x5F) && (gpu_unai.PacketBuffer.U1[1]>0x5F) && (gpu_unai.PacketBuffer.U1[2]>0x5F)))
            driver_idx |= Lighting;
        }

        PP driver = gpuPolySpanDrivers[driver_idx];
        gpuDrawPolyFT(packet, driver, true); // is_quad = true
      }
      break;

      case 0x30:
      case 0x31:
      case 0x32:
      case 0x33:            // Gouraud-shaded 3-pt poly
      {
        //NOTE: The '129' here is CF_GOURAUD | CF_LIGHT, however
        // this is an untextured poly, so CF_LIGHT (texture blend)
        // shouldn't apply. Until the original array of template
        // instantiation ptrs is fixed, we're stuck with this. (TODO)
        PP driver = gpuPolySpanDrivers[
             (gpu_unai.blit_mask?1024:0) |
             Dithering |
             Blending_Mode |
             gpu_unai.Masking | Blending | 129 | gpu_unai.PixelMSB
           ];
        gpuDrawPolyG(packet, driver, false);
      }
      break;

      case 0x34:
      case 0x35:
      case 0x36:
      case 0x37:            // Gouraud-shaded, textured 3-pt poly
      {
        gpuSetCLUT    (gpu_unai.PacketBuffer.U4[2] >> 16);
        gpuSetTexture (gpu_unai.PacketBuffer.U4[5] >> 16);
        PP driver = gpuPolySpanDrivers[
             (gpu_unai.blit_mask?1024:0) |
             Dithering |
             Blending_Mode | gpu_unai.TEXT_MODE |
             gpu_unai.Masking | Blending | ((Lighting)?129:0) | gpu_unai.PixelMSB
           ];
        gpuDrawPolyGT(packet, driver, false);
      }
      break;

      case 0x38:
      case 0x39:
      case 0x3A:
      case 0x3B:            // Gouraud-shaded 4-pt poly
      {
        // See notes regarding '129' for 0x30..0x33 further above -senquack
        PP driver = gpuPolySpanDrivers[
             (gpu_unai.blit_mask?1024:0) |
             Dithering |
             Blending_Mode |
             gpu_unai.Masking | Blending | 129 | gpu_unai.PixelMSB
           ];
        gpuDrawPolyG(packet, driver, true); // is_quad = true
      }
      break;

      case 0x3C:
      case 0x3D:
      case 0x3E:
      case 0x3F:            // Gouraud-shaded, textured 4-pt poly
      {
        gpuSetCLUT    (gpu_unai.PacketBuffer.U4[2] >> 16);
        gpuSetTexture (gpu_unai.PacketBuffer.U4[5] >> 16);
        PP driver = gpuPolySpanDrivers[
             (gpu_unai.blit_mask?1024:0) |
             Dithering |
             Blending_Mode | gpu_unai.TEXT_MODE |
             gpu_unai.Masking | Blending | ((Lighting)?129:0) | gpu_unai.PixelMSB
           ];
        gpuDrawPolyGT(packet, driver, true); // is_quad = true
      }
      break;

      case 0x40:
      case 0x41:
      case 0x42:
      case 0x43:            // Monochrome line
      {
        // Shift index right by one, as untextured prims don't use lighting
        u32 driver_idx = (Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1;
        PSD driver = gpuPixelSpanDrivers[driver_idx];
        gpuDrawLineF(packet, driver);
      }
      break;

      case 0x48 ... 0x4F:   // Monochrome line strip
      {
        u32 num_vertexes = 1;
        u32 *list_position = &(list[2]);

        // Shift index right by one, as untextured prims don't use lighting
        u32 driver_idx = (Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1;
        PSD driver = gpuPixelSpanDrivers[driver_idx];
        gpuDrawLineF(packet, driver);

        while(1)
        {
          gpu_unai.PacketBuffer.U4[1] = gpu_unai.PacketBuffer.U4[2];
          gpu_unai.PacketBuffer.U4[2] = *list_position++;
          gpuDrawLineF(packet, driver);

          num_vertexes++;
          if(list_position >= list_end)
          {
            cmd = -1;
            goto breakloop;
          }
          if((*list_position & 0xf000f000) == 0x50005000)
            break;
        }

        len += (num_vertexes - 2);
      }
      break;

      case 0x50:
      case 0x51:
      case 0x52:
      case 0x53:            // Gouraud-shaded line
      {
        // Shift index right by one, as untextured prims don't use lighting
        u32 driver_idx = (Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1;
        // Index MSB selects Gouraud-shaded PixelSpanDriver:
        driver_idx |= (1 << 5);
        PSD driver = gpuPixelSpanDrivers[driver_idx];
        gpuDrawLineG(packet, driver);
      }
      break;

      case 0x58 ... 0x5F:   // Gouraud-shaded line strip
      {
        u32 num_vertexes = 1;
        u32 *list_position = &(list[2]);

        // Shift index right by one, as untextured prims don't use lighting
        u32 driver_idx = (Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1;
        // Index MSB selects Gouraud-shaded PixelSpanDriver:
        driver_idx |= (1 << 5);
        PSD driver = gpuPixelSpanDrivers[driver_idx];
        gpuDrawLineG(packet, driver);

        while(1)
        {
          gpu_unai.PacketBuffer.U4[0] = gpu_unai.PacketBuffer.U4[2];
          gpu_unai.PacketBuffer.U4[1] = gpu_unai.PacketBuffer.U4[3];
          gpu_unai.PacketBuffer.U4[2] = *list_position++;
          gpu_unai.PacketBuffer.U4[3] = *list_position++;
          gpuDrawLineG(packet, driver);

          num_vertexes++;
          if(list_position >= list_end)
          {
            cmd = -1;
            goto breakloop;
          }
          if((*list_position & 0xf000f000) == 0x50005000)
            break;
        }

        len += (num_vertexes - 2) * 2;
      }
      break;

      case 0x60:
      case 0x61:
      case 0x62:
      case 0x63:            // Monochrome rectangle (variable size)
      {
        PT driver = gpuTileSpanDrivers[(Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1];
        gpuDrawT(packet, driver);
      }
      break;

      case 0x64:
      case 0x65:
      case 0x66:
      case 0x67:            // Textured rectangle (variable size)
      {
        gpuSetCLUT    (gpu_unai.PacketBuffer.U4[2] >> 16);
        u32 driver_idx = Blending_Mode | gpu_unai.TEXT_MODE | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>1);

        //senquack - Only color 808080h-878787h allows skipping lighting calculation:
        // This fixes Silent Hill running animation on loading screens:
        // (On PSX, color values 0x00-0x7F darken the source texture's color,
        //  0x81-FF lighten textures (ultimately clamped to 0x1F),
        //  0x80 leaves source texture color unchanged, HOWEVER,
        //   gpu_unai uses a simple lighting LUT whereby only the upper
        //   5 bits of an 8-bit color are used, so 0x80-0x87 all behave as
        //   0x80.
        //
        // NOTE: I've changed all textured sprite draw commands here and
        //  elsewhere to use proper behavior, but left poly commands
        //  alone, I don't want to slow rendering down too much. (TODO)
        //if ((gpu_unai.PacketBuffer.U1[0]>0x5F) && (gpu_unai.PacketBuffer.U1[1]>0x5F) && (gpu_unai.PacketBuffer.U1[2]>0x5F))
        // Strip lower 3 bits of each color and determine if lighting should be used:
        if ((gpu_unai.PacketBuffer.U4[0] & 0xF8F8F8) != 0x808080)
          driver_idx |= Lighting;
        PS driver = gpuSpriteSpanDrivers[driver_idx];
        gpuDrawS(packet, driver);
      }
      break;

      case 0x68:
      case 0x69:
      case 0x6A:
      case 0x6B:            // Monochrome rectangle (1x1 dot)
      {
        gpu_unai.PacketBuffer.U4[2] = 0x00010001;
        PT driver = gpuTileSpanDrivers[(Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1];
        gpuDrawT(packet, driver);
      }
      break;

      case 0x70:
      case 0x71:
      case 0x72:
      case 0x73:            // Monochrome rectangle (8x8)
      {
        gpu_unai.PacketBuffer.U4[2] = 0x00080008;
        PT driver = gpuTileSpanDrivers[(Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1];
        gpuDrawT(packet, driver);
      }
      break;

      case 0x74:
      case 0x75:
      case 0x76:
      case 0x77:            // Textured rectangle (8x8)
      {
        gpu_unai.PacketBuffer.U4[3] = 0x00080008;
        gpuSetCLUT    (gpu_unai.PacketBuffer.U4[2] >> 16);
        u32 driver_idx = Blending_Mode | gpu_unai.TEXT_MODE | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>1);

        //senquack - Only color 808080h-878787h allows skipping lighting calculation:
        //if ((gpu_unai.PacketBuffer.U1[0]>0x5F) && (gpu_unai.PacketBuffer.U1[1]>0x5F) && (gpu_unai.PacketBuffer.U1[2]>0x5F))
        // Strip lower 3 bits of each color and determine if lighting should be used:
        if ((gpu_unai.PacketBuffer.U4[0] & 0xF8F8F8) != 0x808080)
          driver_idx |= Lighting;
        PS driver = gpuSpriteSpanDrivers[driver_idx];
        gpuDrawS(packet, driver);
      }
      break;

      case 0x78:
      case 0x79:
      case 0x7A:
      case 0x7B:            // Monochrome rectangle (16x16)
      {
        gpu_unai.PacketBuffer.U4[2] = 0x00100010;
        PT driver = gpuTileSpanDrivers[(Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1];
        gpuDrawT(packet, driver);
      }
      break;

      case 0x7C:
      case 0x7D:
#ifdef __arm__
        if ((gpu_unai.GPU_GP1 & 0x180) == 0 && (gpu_unai.Masking | gpu_unai.PixelMSB) == 0)
        {
          gpuSetCLUT    (gpu_unai.PacketBuffer.U4[2] >> 16);
          gpuDrawS16(packet);
          break;
        }
        // fallthrough
#endif
      case 0x7E:
      case 0x7F:            // Textured rectangle (16x16)
      {
        gpu_unai.PacketBuffer.U4[3] = 0x00100010;
        gpuSetCLUT    (gpu_unai.PacketBuffer.U4[2] >> 16);
        u32 driver_idx = Blending_Mode | gpu_unai.TEXT_MODE | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>1);
        //senquack - Only color 808080h-878787h allows skipping lighting calculation:
        //if ((gpu_unai.PacketBuffer.U1[0]>0x5F) && (gpu_unai.PacketBuffer.U1[1]>0x5F) && (gpu_unai.PacketBuffer.U1[2]>0x5F))
        // Strip lower 3 bits of each color and determine if lighting should be used:
        if ((gpu_unai.PacketBuffer.U4[0] & 0xF8F8F8) != 0x808080)
          driver_idx |= Lighting;
        PS driver = gpuSpriteSpanDrivers[driver_idx];
        gpuDrawS(packet, driver);
      }
      break;

      case 0x80:          //  vid -> vid
        gpuMoveImage(packet);
        break;

#ifdef TEST
      case 0xA0:          //  sys -> vid
      {
        u32 load_width = list[2] & 0xffff;
        u32 load_height = list[2] >> 16;
        u32 load_size = load_width * load_height;

        len += load_size / 2;
      }
      break;

      case 0xC0:
        break;
#else
      case 0xA0:          //  sys ->vid
      case 0xC0:          //  vid -> sys
        // Handled by gpulib
        goto breakloop;
#endif
      case 0xE1 ... 0xE6:   // Draw settings
      {
        gpuGP0Cmd_0xEx(gpu_unai, gpu_unai.PacketBuffer.U4[0]);
      }
      break;
    }
  }

breakloop:
  gpu.ex_regs[1] &= ~0x1ff;
  gpu.ex_regs[1] |= gpu_unai.GPU_GP1 & 0x1ff;

  *last_cmd = cmd;
  return list - list_start;
}

void renderer_sync_ecmds(uint32_t *ecmds)
{
  int dummy;
  do_cmd_list(&ecmds[1], 6, &dummy);
}

void renderer_update_caches(int x, int y, int w, int h)
{
}

void renderer_flush_queues(void)
{
}

void renderer_set_interlace(int enable, int is_odd)
{
}

// Handle any gpulib settings applicable to gpu_unai:
void renderer_set_config(const gpulib_config_t *config)
{
  gpu_unai.vram = (u16*)gpu.vram;
}

// vim:shiftwidth=2:expandtab
