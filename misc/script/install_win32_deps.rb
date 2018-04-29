#!/usr/bin/ruby

require "fileutils"

def copy_dlls(srcdir, destdir, exclude_patterns = [])

  dlls = Hash.new
  valid_dlls = Array.new

  Dir.chdir(srcdir){

    Dir.glob("*.dll").each { |dll|
      dlls[dll] = true
    }
    
    dlls.each_key { |dll| 

      if dll =~ /(.+)d\.dll$/
        release_dll = $1 + ".dll"
        next if dlls[release_dll]
      end
      matched = false
      for pattern in exclude_patterns
        if dll =~ pattern
          matched = true
          break
        end
      end
      
      valid_dlls.push(dll) unless matched
    }
  }

  for dll in valid_dlls
    FileUtils.install(srcdir + "\\" + dll, destdir)
    puts "install " + dll
  end

end

def copy_subdirectories(top, subdirs, destdir)
  for subdir in subdirs
    destsub = File.dirname("#{destdir}/#{subdir}")
    FileUtils.mkdir_p(destsub)
    FileUtils.cp_r("#{top}/#{subdir}", destsub)
    puts "install " + subdir
  end
end

         
VERSION_STR = "__"

tmptop = "excade-#{VERSION_STR}"

FileUtils.rm_rf(tmptop)
FileUtils.cp_r("C:/Program Files/Choreonoid", tmptop)

bindir = tmptop + "\\bin"

copy_dlls("/bin", bindir, [/-vc80-/, /-d-/])
copy_dlls("C:/Program Files/OpenRTM-aist/1.2.0/omniORB/4.2.2_vc141/bin/x86_win32", bindir)
copy_dlls("/bin", bindir)

# copy_dlls("/bin", bindir, [/sDIMS/])

copy_subdirectories("", ["etc", "lib/gtk-2.0", "share/themes"], tmptop)

archive = "excade-#{VERSION_STR}-win32.zip"

FileUtils.rm_f(archive)
command = "zip -r #{archive} #{tmptop}"
puts command
system(command)
