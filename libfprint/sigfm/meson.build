
sigfm_sources = ['sigfm.cpp']

opencv = dependency('opencv4', 'opencv5', required: true)
libsigfm = static_library('sigfm',
        sigfm_sources,
        dependencies: [opencv],
)

doctest = dependency('doctest', required: false)
if doctest.found()
    sigfm_tests = executable('sigfm-tests', ['./tests.cpp'],
        dependencies: [doctest, opencv], link_with: [libsigfm])
endif
