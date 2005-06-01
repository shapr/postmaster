# GNUmakefile -- build Postmaster

##### paths and programs

GHC	 := ghc
OBJDIR	 := .objs
HFLAGS	 := -threaded -Wall -O2 -funbox-strict-fields 	\
	    -odir $(OBJDIR) -hidir $(OBJDIR) 		\
	    -ignore-package blockio  -iblockio		\
	    -ignore-package child    -ichild		\
	    -ignore-package hsdns    -idns  '-\#include <adns.h>' '-\#include <sys/poll.h>' \
	    -ignore-package hsemail  -iemail		\
	    -ignore-package monadenv -imonadenv		\
	    -ignore-package hopenssl -ihopenssl '-\#include <openssl/evp.h>' 	\
	    -isyslog
DOCDIR	 := docs
HADDOCK	 := haddock
HSC2HS	 := hsc2hs
HDI_PATH := http://haskell.org/ghc/docs/latest/html/libraries
HDI_FILE := /usr/local/ghc-current/share/ghc-6.5/html/libraries
HDIFILES := \
  -i $(HDI_PATH)/base,$(HDI_FILE)/base/base.haddock \
  -i $(HDI_PATH)/network,$(HDI_FILE)/network/network.haddock \
  -i $(HDI_PATH)/mtl,$(HDI_FILE)/mtl/mtl.haddock \
  -i $(HDI_PATH)/unix,$(HDI_FILE)/unix/unix.haddock \
  -i $(HDI_PATH)/parsec,$(HDI_FILE)/parsec/parsec.haddock

MONODIRS := blockio child dns email hopenssl monadenv syslog

##### build postmaster binary

.PHONY: all

    # TODO: Postmaster/Meta.hs is missing because
    # Haddock can't deal with it.
SRCS := Postmaster.hs				\
	Postmaster/Base.hs			\
	Postmaster/FSM.hs			\
	Postmaster/FSM/Announce.hs		\
	Postmaster/FSM/DNSResolver.hs		\
	Postmaster/FSM/DataHandler.hs		\
	Postmaster/FSM/EhloPeer.hs		\
	Postmaster/FSM/EventHandler.hs		\
	Postmaster/FSM/HeloName.hs		\
	Postmaster/FSM/MailFrom.hs		\
	Postmaster/FSM/MailID.hs		\
	Postmaster/FSM/PeerAddr.hs		\
	Postmaster/FSM/PeerHelo.hs		\
	Postmaster/FSM/SessionState.hs		\
	Postmaster/FSM/Spooler.hs		\
	Postmaster/IO.hs			\
	Postmaster/Main.hs			\
	blockio/System/IO/Driver.hs		\
	child/Control/Concurrent/Child.hs	\
	child/Control/Timeout.hs		\
	dns/Data/Endian.hs			\
	dns/Network/DNS.hs			\
	dns/Network/DNS/ADNS.hs			\
	dns/Network/DNS/PollResolver.hs		\
	dns/Network/IP/Address.hs		\
	dns/System/Posix/GetTimeOfDay.hs	\
	dns/System/Posix/Poll.hs		\
	email/Text/ParserCombinators/Parsec/Rfc2234.hs	\
	email/Text/ParserCombinators/Parsec/Rfc2821.hs	\
	hopenssl/OpenSSL/Digest.hs			\
	monadenv/Control/Monad/Env.hs			\
	syslog/Syslog.hs

all::	postmaster

postmaster:	$(SRCS) tutorial.lhs
	@if [ ! -d $(OBJDIR) ]; then mkdir $(OBJDIR); fi
	@rm -f $(OBJDIR)/Main.o $(OBJDIR)/Main.hi
	$(GHC) --make $(HFLAGS) -o $@ tutorial.lhs -ladns -lcrypto

%.hs : %.hsc
	$(HSC2HS) $<

##### documentation

.PHONY: docs

docs::		$(DOCDIR)/tutorial.html
	@rm -f $(DOCDIR)/index.html
	@$(MAKE) $(DOCDIR)/index.html

$(DOCDIR)/index.html:	$(SRCS)
	@echo "Build documentation ..."
	@if [ ! -d $(DOCDIR) ]; then mkdir $(DOCDIR); fi
	@$(HADDOCK) $(HDIFILES) -s .. -t Postmaster -h \
		-o $(DOCDIR) $(SRCS)

$(DOCDIR)/tutorial.html:	tutorial.lhs
	@lhs2html $<
	@mv tutorial.html $@

NOTES:		.todo
	@devtodo -T -f all --date-format '%Y-%m-%d'
	@mv TODO $@

##### TAGS file

.PHONY: tags

tags::
	@rm -f TAGS
	@$(MAKE) TAGS

TAGS:		$(SRCS)
	@rm -f tags TAGS
	@echo "Updating TAGS ... "
	@(find . -name "*.hs*" && \
	  find /usr/local/src/ghc-current/libraries -name "*.hs*") \
	   | xargs hasktags
	@rm -f tags

##### distribution

.PHONY: dist mkdist

dist:		clean index.html $(DOCDIR)/index.html $(DOCDIR)/tutorial.html

mkdist:
	@darcs dist --dist-name postmaster-`date --iso-8601`

index.html:	README
	@lhs2html $<
	@sed -e 's%<p>\(.*Homepage\]</a></p>\)%<p class="title">\1%' <$<.html >$@
	@rm -f README.html

##### administrative targets

.PHONY: clean distclean redate init-src

clean::
	@rm -rf $(OBJDIR)
	@rm -f postmaster TODO TAGS tags

distclean::	clean
	@rm -rf $(DOCDIR) index.html
	@rm -f postmaster-*.tar.gz NOTES
	@rm -rf $(MONODIRS)

redate::
	redate Postmaster.hs Postmaster/*.hs Postmaster/*/*.hs tutorial.lhs README

init-src::	$(MONODIRS) $(SRCS)
	@-mkdir $(DOCDIR)
	@rm -f MT/monotonerc
	@ln -s ../.monotonerc MT/monotonerc

$(MONODIRS):
	monotone --db=/home/monodbs/simons.db --branch=to.cryp.hs.$@ co $@
