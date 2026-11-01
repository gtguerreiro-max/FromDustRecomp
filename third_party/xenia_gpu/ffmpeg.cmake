# Generated from xenia/third_party/CMakeLists.txt (FFmpeg section): paths rebased onto ${TP}.
set(XE_HAS_GAS_ASM FALSE)
if(XE_TARGET_AARCH64)
  if(MSVC)
    # MSVC can't assemble GAS-syntax .S files. Use clang as the ASM compiler
    # if available — the resulting .obj files link fine with MSVC-compiled code.
    # Pick clang from the host architecture directory so it can actually
    # run on this machine (clang is a cross-compiler — it handles the
    # target arch via --target regardless of host).
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "ARM64")
      set(_clang_host_dir "ARM64")
    else()
      set(_clang_host_dir "x64")
    endif()
    find_program(CLANG_EXE clang HINTS
      "$ENV{VCINSTALLDIR}/Tools/Llvm/${_clang_host_dir}/bin"
      "$ENV{VCINSTALLDIR}/Tools/Llvm/bin"
      "C:/Program Files/LLVM/bin"
    )
    if(CLANG_EXE)
      set(XE_HAS_GAS_ASM TRUE)
      set(XE_CLANG_ASM "${CLANG_EXE}")
      message(STATUS "Using clang for ARM64 assembly: ${CLANG_EXE}")
    else()
      message(WARNING "clang not found — FFmpeg NEON assembly disabled on MSVC ARM64. "
                      "Install LLVM/clang to enable optimized audio decoding.")
    endif()
  else()
    enable_language(ASM)
    set(XE_HAS_GAS_ASM TRUE)
  endif()
endif()

# Helper: compile GAS-syntax .S files with clang on MSVC, or natively on
# GCC/Clang. Adds the resulting objects to the target.
function(xe_add_gas_sources target)
  if(NOT XE_HAS_GAS_ASM)
    return()
  endif()
  foreach(src ${ARGN})
    if(XE_CLANG_ASM)
      # MSVC: use clang to assemble .S -> .obj
      get_filename_component(src_name "${src}" NAME_WE)
      get_filename_component(src_abs "${src}" ABSOLUTE)
      set(obj "${CMAKE_CURRENT_BINARY_DIR}/${src_name}.obj")
      add_custom_command(
        OUTPUT "${obj}"
        COMMAND "${XE_CLANG_ASM}" --target=aarch64-pc-windows-msvc
                -I "${TP}/FFmpeg"
                -I "${TP}/ffmpeg-xenia"
                -c "${src_abs}" -o "${obj}"
        DEPENDS "${src_abs}"
        COMMENT "Assembling ${src} with clang"
      )
      target_sources(${target} PRIVATE "${obj}")
    else()
      # GCC/Clang: native ASM support
      target_sources(${target} PRIVATE "${src}")
    endif()
  endforeach()
endfunction()

# Common function to apply shared FFmpeg settings to a target.
function(ffmpeg_common target)
  target_compile_definitions(${target} PRIVATE
    HAVE_AV_CONFIG_H
    _USE_MATH_DEFINES
  )
  target_include_directories(${target} PRIVATE
    ${TP}/ffmpeg-xenia
    ${TP}/FFmpeg
  )

  if(WIN32)
    target_include_directories(${target} PRIVATE
      ${TP}/FFmpeg/compat/atomics/win32
    )
    target_link_libraries(${target} PRIVATE bcrypt)
    # Size optimization on Debug/Checked (dead code elimination is mandatory)
    target_compile_options(${target} PRIVATE
      $<$<CONFIG:Debug>:/Os>
      $<$<CONFIG:Checked>:/Os>
    )
  else()
    target_include_directories(${target} PRIVATE
      ${TP}/FFmpeg/compat/atomics/gcc
    )
  endif()

  # Suppress all warnings for FFmpeg code
  if(MSVC)
    target_compile_options(${target} PRIVATE /w)
  else()
    target_compile_options(${target} PRIVATE -w)
  endif()

  # All FFmpeg source is C
  set_target_properties(${target} PROPERTIES LINKER_LANGUAGE C)
endfunction()

# --- libavutil ---
add_library(libavutil STATIC
  ${TP}/FFmpeg/libavutil/adler32.c
  ${TP}/FFmpeg/libavutil/aes.c
  ${TP}/FFmpeg/libavutil/aes_ctr.c
  ${TP}/FFmpeg/libavutil/audio_fifo.c
  ${TP}/FFmpeg/libavutil/avstring.c
  ${TP}/FFmpeg/libavutil/avsscanf.c
  ${TP}/FFmpeg/libavutil/base64.c
  ${TP}/FFmpeg/libavutil/blowfish.c
  ${TP}/FFmpeg/libavutil/bprint.c
  ${TP}/FFmpeg/libavutil/buffer.c
  ${TP}/FFmpeg/libavutil/cast5.c
  ${TP}/FFmpeg/libavutil/camellia.c
  ${TP}/FFmpeg/libavutil/channel_layout.c
  ${TP}/FFmpeg/libavutil/csp.c
  ${TP}/FFmpeg/libavutil/cpu.c
  ${TP}/FFmpeg/libavutil/crc.c
  ${TP}/FFmpeg/libavutil/des.c
  ${TP}/FFmpeg/libavutil/dict.c
  ${TP}/FFmpeg/libavutil/display.c
  ${TP}/FFmpeg/libavutil/dovi_meta.c
  ${TP}/FFmpeg/libavutil/downmix_info.c
  ${TP}/FFmpeg/libavutil/encryption_info.c
  ${TP}/FFmpeg/libavutil/error.c
  ${TP}/FFmpeg/libavutil/eval.c
  ${TP}/FFmpeg/libavutil/fifo.c
  ${TP}/FFmpeg/libavutil/file.c
  ${TP}/FFmpeg/libavutil/file_open.c
  ${TP}/FFmpeg/libavutil/float_dsp.c
  ${TP}/FFmpeg/libavutil/fixed_dsp.c
  ${TP}/FFmpeg/libavutil/frame.c
  ${TP}/FFmpeg/libavutil/hash.c
  ${TP}/FFmpeg/libavutil/hdr_dynamic_metadata.c
  ${TP}/FFmpeg/libavutil/hmac.c
  ${TP}/FFmpeg/libavutil/hwcontext.c
  ${TP}/FFmpeg/libavutil/imgutils.c
  ${TP}/FFmpeg/libavutil/integer.c
  ${TP}/FFmpeg/libavutil/intmath.c
  ${TP}/FFmpeg/libavutil/lfg.c
  ${TP}/FFmpeg/libavutil/lls.c
  ${TP}/FFmpeg/libavutil/log.c
  ${TP}/FFmpeg/libavutil/log2_tab.c
  ${TP}/FFmpeg/libavutil/mathematics.c
  ${TP}/FFmpeg/libavutil/mastering_display_metadata.c
  ${TP}/FFmpeg/libavutil/md5.c
  ${TP}/FFmpeg/libavutil/mem.c
  ${TP}/FFmpeg/libavutil/murmur3.c
  ${TP}/FFmpeg/libavutil/opt.c
  ${TP}/FFmpeg/libavutil/parseutils.c
  ${TP}/FFmpeg/libavutil/pixdesc.c
  ${TP}/FFmpeg/libavutil/pixelutils.c
  ${TP}/FFmpeg/libavutil/random_seed.c
  ${TP}/FFmpeg/libavutil/rational.c
  ${TP}/FFmpeg/libavutil/reverse.c
  ${TP}/FFmpeg/libavutil/rc4.c
  ${TP}/FFmpeg/libavutil/ripemd.c
  ${TP}/FFmpeg/libavutil/samplefmt.c
  ${TP}/FFmpeg/libavutil/sha.c
  ${TP}/FFmpeg/libavutil/sha512.c
  ${TP}/FFmpeg/libavutil/slicethread.c
  ${TP}/FFmpeg/libavutil/spherical.c
  ${TP}/FFmpeg/libavutil/stereo3d.c
  ${TP}/FFmpeg/libavutil/threadmessage.c
  ${TP}/FFmpeg/libavutil/time.c
  ${TP}/FFmpeg/libavutil/timecode.c
  ${TP}/FFmpeg/libavutil/tree.c
  ${TP}/FFmpeg/libavutil/twofish.c
  ${TP}/FFmpeg/libavutil/utils.c
  ${TP}/FFmpeg/libavutil/xga_font_data.c
  ${TP}/FFmpeg/libavutil/xtea.c
  ${TP}/FFmpeg/libavutil/tea.c
  ${TP}/FFmpeg/libavutil/tx.c
  ${TP}/FFmpeg/libavutil/tx_float.c
  ${TP}/FFmpeg/libavutil/tx_double.c
  ${TP}/FFmpeg/libavutil/tx_int32.c
  ${TP}/FFmpeg/libavutil/video_enc_params.c
  ${TP}/FFmpeg/libavutil/film_grain_params.c
  ${TP}/FFmpeg/libavutil/side_data.c
  ${TP}/FFmpeg/libavutil/refstruct.c
  ${TP}/FFmpeg/libavutil/container_fifo.c
  ${TP}/FFmpeg/libavutil/float_scalarproduct.c
  ${TP}/FFmpeg/libavutil/timecode_internal.c
  ${TP}/FFmpeg/libavutil/iamf.c
  ${TP}/FFmpeg/libavutil/timestamp.c
)
# x86 platform files
if(XE_TARGET_X86_64)
  target_sources(libavutil PRIVATE
    ${TP}/FFmpeg/libavutil/x86/cpu.c
    ${TP}/FFmpeg/libavutil/x86/fixed_dsp_init.c
    ${TP}/FFmpeg/libavutil/x86/float_dsp_init.c
    ${TP}/FFmpeg/libavutil/x86/imgutils_init.c
    ${TP}/FFmpeg/libavutil/x86/lls_init.c
    ${TP}/FFmpeg/libavutil/x86/tx_float_init.c
  )
endif()
# aarch64 platform files
if(XE_TARGET_AARCH64)
  target_sources(libavutil PRIVATE
    ${TP}/FFmpeg/libavutil/aarch64/cpu.c
  )
  if(XE_HAS_GAS_ASM)
    # NEON init files reference symbols from the .S files, so both must
    # be included together. Requires a GAS-compatible assembler (gcc/clang).
    target_sources(libavutil PRIVATE
      ${TP}/FFmpeg/libavutil/aarch64/float_dsp_init.c
      ${TP}/FFmpeg/libavutil/aarch64/tx_float_init.c
    )
    xe_add_gas_sources(libavutil
      ${TP}/FFmpeg/libavutil/aarch64/float_dsp_neon.S
      ${TP}/FFmpeg/libavutil/aarch64/tx_float_neon.S
    )
  endif()
endif()
ffmpeg_common(libavutil)

# --- libavcodec ---
add_library(libavcodec STATIC
  ${TP}/FFmpeg/libavcodec/ac3_parser.c
  ${TP}/FFmpeg/libavcodec/adts_parser.c
  ${TP}/FFmpeg/libavcodec/allcodecs.c
  ${TP}/FFmpeg/libavcodec/avcodec.c
  ${TP}/FFmpeg/libavcodec/avdct.c
  ${TP}/FFmpeg/libavcodec/packet.c
  ${TP}/FFmpeg/libavcodec/bitstream.c
  ${TP}/FFmpeg/libavcodec/bitstream_filters.c
  ${TP}/FFmpeg/libavcodec/bsf.c
  ${TP}/FFmpeg/libavcodec/codec_desc.c
  ${TP}/FFmpeg/libavcodec/codec_par.c
  ${TP}/FFmpeg/libavcodec/d3d11va.c
  ${TP}/FFmpeg/libavcodec/decode.c
  ${TP}/FFmpeg/libavcodec/dirac.c
  ${TP}/FFmpeg/libavcodec/dv_profile.c
  ${TP}/FFmpeg/libavcodec/encode.c
  ${TP}/FFmpeg/libavcodec/imgconvert.c
  ${TP}/FFmpeg/libavcodec/jni.c
  ${TP}/FFmpeg/libavcodec/mathtables.c
  ${TP}/FFmpeg/libavcodec/mediacodec.c
  ${TP}/FFmpeg/libavcodec/mpeg12framerate.c
  ${TP}/FFmpeg/libavcodec/options.c
  ${TP}/FFmpeg/libavcodec/parser.c
  ${TP}/FFmpeg/libavcodec/parsers.c
  ${TP}/FFmpeg/libavcodec/profiles.c
  ${TP}/FFmpeg/libavcodec/qsv_api.c
  ${TP}/FFmpeg/libavcodec/raw.c
  ${TP}/FFmpeg/libavcodec/utils.c
  ${TP}/FFmpeg/libavcodec/vorbis_parser.c
  ${TP}/FFmpeg/libavcodec/xiph.c
  ${TP}/FFmpeg/libavcodec/dct32_fixed.c
  ${TP}/FFmpeg/libavcodec/dct32_float.c
  ${TP}/FFmpeg/libavcodec/faandct.c
  ${TP}/FFmpeg/libavcodec/faanidct.c
  ${TP}/FFmpeg/libavcodec/fdctdsp.c
  ${TP}/FFmpeg/libavcodec/jfdctfst.c
  ${TP}/FFmpeg/libavcodec/jfdctint.c
  ${TP}/FFmpeg/libavcodec/idctdsp.c
  ${TP}/FFmpeg/libavcodec/simple_idct.c
  ${TP}/FFmpeg/libavcodec/jrevdct.c
  ${TP}/FFmpeg/libavcodec/mpegaudio.c
  ${TP}/FFmpeg/libavcodec/mpegaudiodec_common.c
  ${TP}/FFmpeg/libavcodec/mpegaudiodsp.c
  ${TP}/FFmpeg/libavcodec/mpegaudiodsp_data.c
  ${TP}/FFmpeg/libavcodec/mpegaudiodsp_fixed.c
  ${TP}/FFmpeg/libavcodec/mpegaudiodsp_float.c
  ${TP}/FFmpeg/libavcodec/mpegaudiodecheader.c
  ${TP}/FFmpeg/libavcodec/mpegaudiodata.c
  ${TP}/FFmpeg/libavcodec/sinewin.c
  ${TP}/FFmpeg/libavcodec/wma_freqs.c
  ${TP}/FFmpeg/libavcodec/mpegaudiodec_fixed.c
  ${TP}/FFmpeg/libavcodec/mpegaudiodec_float.c
  ${TP}/FFmpeg/libavcodec/wmaprodec.c
  ${TP}/FFmpeg/libavcodec/wma.c
  ${TP}/FFmpeg/libavcodec/wma_common.c
  ${TP}/FFmpeg/libavcodec/wmadec.c
  ${TP}/FFmpeg/libavcodec/aactab.c
  ${TP}/FFmpeg/libavcodec/mpegaudio_parser.c
  ${TP}/FFmpeg/libavcodec/pthread.c
  ${TP}/FFmpeg/libavcodec/pthread_slice.c
  ${TP}/FFmpeg/libavcodec/pthread_frame.c
  ${TP}/FFmpeg/libavcodec/get_buffer.c
  ${TP}/FFmpeg/libavcodec/vlc.c
  ${TP}/FFmpeg/libavcodec/threadprogress.c
  ${TP}/FFmpeg/libavcodec/bsf/null.c
  ${TP}/FFmpeg/libavcodec/exif.c
  ${TP}/FFmpeg/libavcodec/mpegaudiotabs.c
  ${TP}/FFmpeg/libavcodec/tiff_common.c
)
if(WIN32)
  target_sources(libavcodec PRIVATE ${TP}/FFmpeg/libavcodec/file_open.c)
endif()
# x86 platform files
if(XE_TARGET_X86_64)
  target_sources(libavcodec PRIVATE
    ${TP}/FFmpeg/libavcodec/x86/constants.c
    ${TP}/FFmpeg/libavcodec/x86/fdctdsp_init.c
    ${TP}/FFmpeg/libavcodec/x86/idctdsp_init.c
    ${TP}/FFmpeg/libavcodec/x86/mpegaudiodsp.c
    ${TP}/FFmpeg/libavcodec/x86/fdct.c
  )
endif()
# aarch64 platform files
if(XE_TARGET_AARCH64)
  if(XE_HAS_GAS_ASM)
    target_sources(libavcodec PRIVATE
      ${TP}/FFmpeg/libavcodec/aarch64/idctdsp_init_aarch64.c
      ${TP}/FFmpeg/libavcodec/aarch64/mpegaudiodsp_init.c
    )
    xe_add_gas_sources(libavcodec
      ${TP}/FFmpeg/libavcodec/aarch64/idctdsp_neon.S
      ${TP}/FFmpeg/libavcodec/aarch64/mpegaudiodsp_neon.S
      ${TP}/FFmpeg/libavcodec/aarch64/simple_idct_neon.S
    )
  endif()
endif()
target_include_directories(libavcodec PRIVATE
  ${TP}/FFmpeg/libavcodec
)
target_link_libraries(libavcodec PRIVATE libavutil)
ffmpeg_common(libavcodec)

# --- libavformat ---
add_library(libavformat STATIC
  ${TP}/FFmpeg/libavformat/allformats.c
  ${TP}/FFmpeg/libavformat/avio.c
  ${TP}/FFmpeg/libavformat/aviobuf.c
  ${TP}/FFmpeg/libavformat/dump.c
  ${TP}/FFmpeg/libavformat/format.c
  ${TP}/FFmpeg/libavformat/id3v1.c
  ${TP}/FFmpeg/libavformat/id3v2.c
  ${TP}/FFmpeg/libavformat/metadata.c
  ${TP}/FFmpeg/libavformat/mux.c
  ${TP}/FFmpeg/libavformat/options.c
  ${TP}/FFmpeg/libavformat/protocols.c
  ${TP}/FFmpeg/libavformat/riff.c
  ${TP}/FFmpeg/libavformat/sdp.c
  ${TP}/FFmpeg/libavformat/url.c
  ${TP}/FFmpeg/libavformat/utils.c
  ${TP}/FFmpeg/libavformat/riffdec.c
  ${TP}/FFmpeg/libavformat/asfdec_f.c
  ${TP}/FFmpeg/libavformat/asf.c
  ${TP}/FFmpeg/libavformat/asfcrypt.c
  ${TP}/FFmpeg/libavformat/avlanguage.c
  ${TP}/FFmpeg/libavformat/mp3dec.c
  ${TP}/FFmpeg/libavformat/replaygain.c
  ${TP}/FFmpeg/libavformat/file.c
  ${TP}/FFmpeg/libavformat/demux.c
  ${TP}/FFmpeg/libavformat/avformat.c
  ${TP}/FFmpeg/libavformat/seek.c
  ${TP}/FFmpeg/libavformat/demux_utils.c
  ${TP}/FFmpeg/libavformat/urldecode.c
  ${TP}/FFmpeg/libavformat/asf_tags.c
  ${TP}/FFmpeg/libavformat/to_upper4.c
)
if(WIN32)
  target_sources(libavformat PRIVATE ${TP}/FFmpeg/libavformat/file_open.c)
endif()
target_link_libraries(libavformat PRIVATE libavutil libavcodec)
ffmpeg_common(libavformat)
