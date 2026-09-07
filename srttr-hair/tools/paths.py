"""Source packfile resolution.

Installing writes the rebuilt packfile over the game's customize_item.vpp_pc, so the
live file is NOT a safe build input - building from it reshapes an already-reshaped
mesh and compounds the deformation. install.ps1 always leaves the pristine original
at customize_item.vpp_pc.bak, so that is preferred whenever it exists.
"""
import os

GAME_DIR = r"F:/SteamLibrary/steamapps/common/Saints Row The Third Remastered"
SRTT = r"D:/SR3RTXREMIXCOMP/Saints Row 3/packfiles/pc/cache/customize_item.vpp_pc"


def srttr_stock():
    """The unmodified SRTTR customize_item.vpp_pc."""
    live = os.path.join(GAME_DIR, "cache", "customize_item.vpp_pc")
    bak = live + ".bak"
    return bak if os.path.exists(bak) else live
