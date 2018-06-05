RELEASE=4.1

PACKAGE=cfs
PKGVER=4.0
PKGREL=40

ARCH:=$(shell dpkg-architecture -qDEB_BUILD_ARCH)
GITVERSION:=$(shell cat .git/refs/heads/master)

DEB=${PACKAGE}_${PKGVER}-${PKGREL}_${ARCH}.deb
DBG_DEB=${PACKAGE}-dbg_${PKGVER}-${PKGREL}_${ARCH}.deb



all: ${DEB} ${DBG_DEB}

.PHONY: dinstall
dinstall: ${DEB}
	dpkg -i ${DEB} 

.PHONY: ${DEB} ${DBG_DEB}
${DEB} ${DBG_DEB}:
	rm -rf build
	rsync -a --exclude .svn data/ build
	cp -a debian build/debian
	cd build; ./autogen.sh
	cd build; dpkg-buildpackage -rfakeroot -b -us -uc
	lintian ${DEB}


.PHONY: upload
upload: ${DEB} ${DBG_DEB}
	umount /pve/${RELEASE}; mount /pve/${RELEASE} -o rw 
	mkdir -p /pve/${RELEASE}/extra
	rm -f /pve/${RELEASE}/extra/${PACKAGE}*.deb
	rm -f /pve/${RELEASE}/extra/Packages*
	cp ${DEB} ${DBG_DEB} /pve/${RELEASE}/extra
	cd /pve/${RELEASE}/extra; dpkg-scanpackages . /dev/null > Packages; gzip -9c Packages > Packages.gz
	umount /pve/${RELEASE}; mount /pve/${RELEASE} -o ro

.PHONY: clean
clean:
	rm -rf *~ build *_${ARCH}.deb *.changes *.dsc ${CSDIR}
