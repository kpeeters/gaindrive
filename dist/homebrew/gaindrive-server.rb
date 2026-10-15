# Source copy of the formula in the kpeeters/repo tap. Named gaindrive-server
# rather than gaindrive because the macOS client app takes that name; Homebrew
# derives the formula name from the file name, so keep the two in step.
class GaindriveServer < Formula
  desc "Music and video streaming server with a Subsonic-compatible API"
  homepage "https://github.com/kpeeters/gaindrive"
  url "https://github.com/kpeeters/gaindrive/archive/refs/tags/v1.0.8.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "GPL-3.0-or-later"
  head "https://github.com/kpeeters/gaindrive.git", branch: "master"

  depends_on "cmake" => :build
  depends_on "cxxopts" => :build
  depends_on "nlohmann-json" => :build
  depends_on "pkgconf" => :build
  depends_on arch: :arm64
  # ffmpeg is linked and also run as ffmpeg/ffprobe; the service PATH includes
  # brew's bin, so the subprocesses find the same build.
  depends_on "ffmpeg"
  depends_on "libarchive"
  depends_on "openssl@3"
  depends_on "reproc"
  depends_on "sqlitecpp"
  depends_on "taglib"
  depends_on "tinyxml2"

  def install
    # SYSCONFDIR and LOCALSTATEDIR are compiled in as the --config and --db
    # defaults, so the service needs no arguments. libarchive is named
    # explicitly because macOS ships an older one in /usr/lib that would
    # otherwise be found first.
    args = %W[
      -DGAINDRIVE_VERSION=#{version}
      -DGAINDRIVE_ALLOW_FETCH=OFF
      -DGAINDRIVE_CAST_TOOL=OFF
      -DGAINDRIVE_SYSCONFDIR=#{etc}
      -DGAINDRIVE_LOCALSTATEDIR=#{var}/lib
      -DOPENSSL_ROOT_DIR=#{Formula["openssl@3"].opt_prefix}
      -DCMAKE_PREFIX_PATH=#{Formula["libarchive"].opt_prefix}
    ]
    system "cmake", "-S", ".", "-B", "build", *args, *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  def post_install
    (var/"lib/gaindrive").mkpath
  end

  def caveats
    <<~EOS
      gaindrive reads #{etc}/gaindrive.conf, which is not created for you.
      Start from the annotated example:
        cp #{etc}/gaindrive.conf.example #{etc}/gaindrive.conf
        chmod 0640 #{etc}/gaindrive.conf
      Set "roots" to your library. The example's /var/lib and /tmp paths
      (db_path, user_db_path, transcode_cache_dir, upload_dir and the uploads
      root) override the Homebrew defaults under #{var}/lib/gaindrive, so
      change or delete them.

      Create the first user, then start the service:
        gaindrive --add-user NAME --password PASSWORD
        brew services start gaindrive-server
    EOS
  end

  service do
    run opt_bin/"gaindrive"
    keep_alive true
    working_dir var/"lib/gaindrive"
    log_path var/"log/gaindrive.log"
    error_log_path var/"log/gaindrive.log"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/gaindrive --version")
  end
end
