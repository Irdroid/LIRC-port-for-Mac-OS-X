## $Id: acinclude.m4,v 1.1 1999/07/27 16:56:35 columbus Exp $
##
## additional m4 macros
##
## (C) 1999 Christoph Bartelmus (lirc@bartelmus.de)
##


dnl check for kernel source

AC_DEFUN(AC_PATH_KERNEL_SOURCE,
[
  AC_MSG_CHECKING(for Linux kernel sources)
  AC_CACHE_VAL(ac_cv_have_kernel,[
    kerneldir=missing
    no_kernel=yes

    if test `uname` != "Linux"; then
      kerneldir="not running Linux"
    elif test -d /usr/src/linux-`uname -r`/; then
      kerneldir=/usr/src/linux-`uname -r`/
      no_kernel=no
    elif test -d /usr/src/linux/; then
      kerneldir=/usr/src/linux/
      no_kernel=no
    fi

    if test x${no_kernel} != xyes; then
      if test -f ${kerneldir}/Makefile; then
dnl         kernelcc=`grep "^CC.*=" ${kerneldir}/Makefile|sed -e "s/=/=\'/g" -e "s/	//g" -e "s/(/\\\\\\(/g" -e "s/)/\\\\\\)/g"`\'
        kernelcc=`grep "^CC.*=" ${kerneldir}/Makefile|sed -e "s/=/=\'/g" -e "s/	//g" -e 's/\\$/\\\\$/g'`\'
        ac_save_cc="${CC}"
        eval ${kernelcc}
        kernelcc="${CC}"
        CC="${ac_save_cc}"
      else
        kerneldir="no Makefile found"
        no_kernel=yes
      fi
    fi
    ac_cv_have_kernel="no_kernel=${no_kernel} \
		kerneldir=\"${kerneldir}\" \
		kernelcc=\"${kernelcc}\""
    ]
  )
  
  eval "$ac_cv_have_kernel"

  AC_SUBST(kerneldir)
  AC_SUBST(kernelcc)
  AC_MSG_RESULT(${kerneldir})
]
)
