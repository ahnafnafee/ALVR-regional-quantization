"""Read the local importance bridge without creating a mapping or changing settings."""
import argparse
import ctypes
from ctypes import wintypes
import json
import struct
import time
from pathlib import Path

kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
kernel.OpenFileMappingW.restype = wintypes.HANDLE
kernel.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
kernel.MapViewOfFile.restype = ctypes.c_void_p
kernel.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.OpenMutexW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
kernel.OpenMutexW.restype = wintypes.HANDLE
kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel.ReleaseMutex.argtypes = [wintypes.HANDLE]


def snapshot():
    handle = kernel.OpenFileMappingW(4, False, 'Local\\GVL.StreamingAttention.v1')
    if not handle:
        return {'available': False}
    mutex = kernel.OpenMutexW(0x100001, False, 'Local\\GVL.StreamingAttention.v1.Lock')
    view = kernel.MapViewOfFile(handle, 4, 0, 0, 8192)
    try:
        if not view or not mutex or kernel.WaitForSingleObject(mutex, 0) not in (0, 128):
            return {'available': True, 'busy': True}
        try:
            data = ctypes.string_at(view, 128)
            quality = ctypes.string_at(view + 6528, 184)
        finally:
            kernel.ReleaseMutex(mutex)
        names = ['magic', 'version', 'consumer_pid', 'enabled', 'heartbeat', 'last_sequence', 'applied', 'rejected',
                 'protected_fraction', 'min_qp_delta', 'max_qp_delta', 'width', 'height', 'block', 'encoder_error', 'scene_pid',
                 'map_build_ms', 'regions', 'producer_age_ms', 'producer_pid', 'publisher_ms', 'publisher_alloc_bytes',
                 'high_priority_fraction', 'intermediate_fraction', 'background_fraction', 'requested_qpc']
        result = dict(zip(names, struct.unpack('<4I4Qf2i5IfIfIfq3fQ', data)))
        header = struct.unpack_from('<4I2Q', quality)
        if header[:3] == (0x31514c47, 1, 184):
            fields = ['state', 'feedback_count', 'invalid_feedback', 'feedback_fresh', 'source_timestamp', 'feedback_qpc',
                      'network_residual_ms', 'excess_ms', 'decoder_queue_ms', 'jitter_ms', 'recommended_extra_qp',
                      'recommended_bitrate_scale', 'applied_extra_qp', 'applied_bitrate_scale', 'nominal_mbps',
                      'applied_mbps', 'encoded_frame_kbytes', 'frame_interval_ms', 'packets_lost_total']
            status = dict(zip(fields, struct.unpack_from('<4I2Q12fQ', quality, 96)))
            mode = struct.unpack_from('<I', quality, 32)[0]
            status['mode'] = ['disabled', 'observe', 'adaptive'][mode] if mode < 3 else 'invalid'
            producer = struct.unpack_from('<I', quality, 64)[0]
            updated = struct.unpack_from('<Q', quality, 72)[0]
            qpc = ctypes.c_longlong()
            kernel.QueryPerformanceCounter(ctypes.byref(qpc))
            age = (qpc.value - updated) * 1000 / header[5] if header[5] and updated else None
            status['config_producer_pid'] = producer
            status['config_age_ms'] = age
            status['config_lease_valid'] = producer == result['scene_pid'] and producer != 0 and age is not None and 0 <= age <= 2500
            result['regional_quality'] = status
        result['available'] = True
        return result
    finally:
        if view: kernel.UnmapViewOfFile(view)
        if mutex: kernel.CloseHandle(mutex)
        kernel.CloseHandle(handle)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--seconds', type=int, default=1)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    rows = []
    for i in range(max(1, args.seconds)):
        rows.append(snapshot())
        print(json.dumps(rows[-1]), flush=True)
        if i + 1 < args.seconds: time.sleep(1)
    if args.output: args.output.write_text(json.dumps(rows, indent=2) + '\n', encoding='utf-8')
