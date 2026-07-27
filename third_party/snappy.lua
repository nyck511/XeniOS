group("third_party")
project("snappy")
  uuid("bb143d61-3fd4-44c2-8b7e-04cc538ba2c7")
  kind("StaticLib")
  language("C++")

  files({
    "snappy/snappy-internal.h",
    "snappy/snappy-sinksource.cc",
    "snappy/snappy-sinksource.h",
    "snappy/snappy-stubs-internal.cc",
    "snappy/snappy-stubs-internal.h",
    "snappy/snappy-stubs-public.h",
    "snappy/snappy.cc",
    "snappy/snappy.h",
  })

  -- Disable AVX requirement to prevent BMI2 instructions from being baked in at compile time
  -- This allows Snappy to work on CPUs without BMI2 support (e.g., Sandy Bridge, Ivy Bridge)
  local snappy_dir = path.getabsolute("snappy")
  local snappy_stubs_path = path.join(snappy_dir, "snappy-stubs-public.h")
  if not os.isfile(snappy_stubs_path) then
    local snappy_stubs_template =
        io.readfile(path.join(snappy_dir, "snappy-stubs-public.h.in"))
    local snappy_cmake = io.readfile(path.join(snappy_dir, "CMakeLists.txt"))
    if not snappy_stubs_template or not snappy_cmake then
      error("Unable to read Snappy build inputs")
    end
    local version_major, version_minor, version_patch =
        snappy_cmake:match(
            "project%(%s*Snappy%s+VERSION%s+(%d+)%.(%d+)%.(%d+)")
    if not version_major then
      error("Unable to generate snappy-stubs-public.h")
    end

    local have_sys_uio_h = os.istarget("windows") and "0" or "1"
    local snappy_stubs = snappy_stubs_template
        :gsub("%${HAVE_SYS_UIO_H_01}", have_sys_uio_h)
        :gsub("%${PROJECT_VERSION_MAJOR}", version_major)
        :gsub("%${PROJECT_VERSION_MINOR}", version_minor)
        :gsub("%${PROJECT_VERSION_PATCH}", version_patch)
    io.writefile(snappy_stubs_path, snappy_stubs)
  end
