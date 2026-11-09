#include "d3d9_hud.h"
#include "../dxvk/hud/dxvk_hud_font.h"

namespace dxvk::hud {

  void HudRenderStack::update(dxvk::high_resolution_clock::time_point time) {
    if (std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate).count() < 200'000)
      return;
    m_snapshot.StructSize = sizeof(m_snapshot);
    m_snapshot.Version = D3D9_BRIDGE_DIAGNOSTICS_VERSION;
    m_device->GetBridgeHudSnapshot(&m_snapshot);
    size_t maxCharacters = 0;
    for (UINT i = 0; i < m_snapshot.LineCount; ++i) {
      m_lines[i] = m_snapshot.Lines[i];
      maxCharacters = std::max(maxCharacters, m_lines[i].size());
    }
    m_textWidth = (maxCharacters * 16 * g_hudFont.advance + g_hudFont.size - 1) / g_hudFont.size;
    m_renderWidth = -1;
    m_lastUpdate = time;
  }

  HudPos HudRenderStack::render(const Rc<DxvkCommandList>& ctx,
    const HudPipelineKey& key, const HudOptions& options, HudRenderer& renderer,
    HudPos position) {
    if (!m_snapshot.Visible)
      return position;
    const auto surface = renderer.getPushConstants().surfaceSize;
    const auto width = int32_t(surface.width / options.scale);
    const auto height = int32_t(surface.height / options.scale);
    // The frame-time graph occupies the bottom 144 logical pixels. When the
    // full statistics column reaches it, continue in a separate right column;
    // following D3D9 memory items inherit that position as well.
    if (position.y + int32_t(m_snapshot.LineCount * 16 + 8) > height - 144
     && width >= 560 + 256 + 16)
      position = { std::max(568, width - m_textWidth - 8), 8 };
    const int32_t available = std::max(0, width - position.x - 8);
    if (m_renderWidth != available) {
      m_renderWidth = available;
      const size_t characters = available * g_hudFont.size / (16 * g_hudFont.advance);
      for (UINT i = 0; i < m_snapshot.LineCount; ++i) {
        m_renderLines[i] = m_lines[i];
        if (m_renderLines[i].size() > characters) {
          // Long symbols remain complete in the report. Cache visible text
          // until the next snapshot/resize, keeping per-frame rendering cheap.
          m_renderLines[i].resize(characters > 3 ? characters - 3 : 0);
          m_renderLines[i].append(std::min(characters, size_t(3)), '.');
        }
      }
    }
    for (UINT i = 0; i < m_snapshot.LineCount; ++i) {
      position.y += 16;
      renderer.drawText(16, position, i ? 0xffffffffu : 0xffc0ff00u, m_renderLines[i]);
    }
    position.y += 8;
    return position;
  }

  HudTextureMemory::HudTextureMemory(D3D9DeviceEx* device)
  : m_device          (device)
  , m_allocatedString ("")
  , m_mappedString    ("") { }


  void HudTextureMemory::update(dxvk::high_resolution_clock::time_point time) {
    auto stats = m_device->GetAllocator()->getStats();

    m_maxAllocated = std::max(m_maxAllocated, stats.memoryAllocated);
    m_maxUsed = std::max(m_maxUsed, stats.memoryUsed);
    m_maxMapped = std::max(m_maxMapped, stats.memoryMapped);

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate);

    if (elapsed.count() < UpdateInterval)
      return;

    m_allocatedString = str::format(m_maxAllocated >> 20, " MB (Used: ", m_maxUsed >> 20, " MB)");
    m_mappedString = str::format(m_maxMapped >> 20, " MB");
    m_maxAllocated = 0;
    m_maxUsed = 0;
    m_maxMapped = 0;
    m_lastUpdate = time;
  }


  HudPos HudTextureMemory::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    position.y += 16;
    renderer.drawText(16, position, 0xffc0ff00u, "Mappable:");
    renderer.drawText(16, { position.x + 120, position.y }, 0xffffffffu, m_allocatedString);

    position.y += 20;
    renderer.drawText(16, position, 0xffc0ff00u, "Mapped:");
    renderer.drawText(16, { position.x + 120, position.y }, 0xffffffffu, m_mappedString);

    position.y += 8;
    return position;
  }


  HudSWVPState::HudSWVPState(D3D9DeviceEx* device)
  : m_device(device)  {

  }


  void HudSWVPState::update(dxvk::high_resolution_clock::time_point time) {
    m_swvpShaderCount = str::format(m_device->GetSWVPShaderCount());

    if (m_device->IsSWVP()) {
      if (m_device->CanOnlySWVP()) {
        m_isSWVPText = "SWVP";
      } else {
        m_isSWVPText = "SWVP (Mixed)";
      }
    } else {
      if (m_device->CanSWVP()) {
        m_isSWVPText = "HWVP (Mixed)";
      } else {
        m_isSWVPText = "HWVP";
      }
    }
  }


  HudPos HudSWVPState::render(
    const Rc<DxvkCommandList>&ctx,
    const HudPipelineKey&     key,
    const HudOptions&         options,
          HudRenderer&        renderer,
          HudPos              position) {
    position.y += 16;
    renderer.drawText(16, position, 0xffc0ff00u, "Vertex Processing:");
    renderer.drawText(16, { position.x + 240, position.y }, 0xffffffffu, m_isSWVPText);

    position.y += 20;
    renderer.drawText(16, position, 0xffc0ff00u, "SWVP Shaders:");
    renderer.drawText(16, { position.x + 168, position.y }, 0xffffffffu, m_swvpShaderCount);

    position.y += 8;
    return position;
  }

}
