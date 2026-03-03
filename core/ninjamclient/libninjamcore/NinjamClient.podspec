Pod::Spec.new do |s|
  s.name             = 'NinjamClient'
  s.version          = '1.0.0'
  s.summary          = 'Cliente Ninjam'
  s.description      = 'Biblioteca cliente Ninjam para iOS'
  s.homepage         = 'https://github.com/jacinside'
  s.license          = { :type => 'MIT' }
  s.author           = { 'jacinside' => 'email@ejemplo.com' }
  s.source           = { :path => '.' }
  s.ios.deployment_target = '15.1'
  s.source_files = '**/*.{h,hpp,c,cpp,mm}'
  
  s.vendored_frameworks = [
    '../libogg.xcframework',
    '../libvorbis.xcframework',
    '../libvorbisenc.xcframework'
  ]
  
  s.pod_target_xcconfig = {
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'HEADER_SEARCH_PATHS' => '$(PODS_ROOT)/../../ios/NativeAudioModule/NJClient $(PODS_ROOT)/../../ios/NativeAudioModule/libogg.xcframework/ios-arm64/Headers $(PODS_ROOT)/../../ios/NativeAudioModule/libvorbis.xcframework/ios-arm64/Headers $(PODS_ROOT)/../../ios/NativeAudioModule/libvorbisenc.xcframework/ios-arm64/Headers'
  }
end
