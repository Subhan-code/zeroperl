use ExtUtils::MakeMaker;

WriteMakefile(
    NAME              => 'AsyncWebAPI',
    VERSION_FROM      => 'AsyncWebAPI.pm',
    PREREQ_PM         => {
        'XSLoader' => 0,
    },
    ABSTRACT_FROM     => 'AsyncWebAPI.pm',
    AUTHOR            => 'zeroperl project',
    CCFLAGS           => '-I./stubs',  # Include path for async_web_api.h
    LIBS              => [''],         # No additional libraries needed
    INC               => '-I./stubs',
    OBJECT            => 'AsyncWebAPI.o',  # Object file to build
    XSOPT             => '-C++',
    OPTIMIZE          => '-O2',
);