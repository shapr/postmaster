{-# OPTIONS -fglasgow-exts #-}
{- |
   Module      :  Postmaster.IO
   Copyright   :  (c) 2005-02-10 by Peter Simons
   License     :  GPL2

   Maintainer  :  simons@cryp.to
   Stability   :  provisional
   Portability :  Haskell 2-pre
 -}

module Postmaster.IO where

import Prelude hiding ( catch )
import Data.Maybe
import Data.Typeable
import Control.Concurrent ( forkIO )
import Control.Exception
import Control.Monad.RWS hiding ( local )
import System.IO
import Network ( listenOn, PortID(..) )
import Network.Socket
import Postmaster.Base
import Rfc2821
import BlockIO
import Child
import MonadEnv

-- * Socket Handlers

type SocketHandler = (Socket,SockAddr) -> IO ()

-- |Creates a listening socket for the given port, then
-- calls 'acceptor' to start the given computation for every
-- incoming connection.

listener :: PortID -> SocketHandler -> IO ()
listener p h = bracket (listenOn p) (sClose) (acceptor h)

-- |Given a listening socket, this function will loop
-- forever 'accept'ing incoming connections. For each
-- connection a 'SocketHandler' thread is forked.

acceptor :: SocketHandler -> Socket -> IO ()
acceptor h ls = do
  bracketOnError
    (accept ls)
    (sClose . fst)
    (\peer@(s,_) -> fork $ h peer `finally` sClose s)
  acceptor h ls
  where
  fork f = forkIO f >> return ()

type LazyHandler = (Handle, Maybe SockAddr) -> IO ()

handleLazy :: IOMode -> LazyHandler -> SocketHandler
handleLazy m f (s,sa) =
  bracket (socketToHandle s m) (hClose) (\h -> f (h, Just sa))


-- * Non-blocking I\/O

-- |The exception we throw when writes time out.
-- 'ReadTimeout' is throw by "BlockIO".

data WriteTimeout = WriteTimeout Timeout
                  deriving (Typeable, Show)

setReadTimeout :: Timeout -> Smtpd ()
setReadTimeout = local . setval (mkVar "ReadTimeout")

getReadTimeout :: Smtpd Timeout
getReadTimeout = local $ getDefault (mkVar "ReadTimeout") (90 * 1000000)

setWriteTimeout :: Timeout -> Smtpd ()
setWriteTimeout = local . setval (mkVar "WriteTimeout")

getWriteTimeout :: Smtpd Timeout
getWriteTimeout = local $ getDefault (mkVar "WriteTimeout") (90 * 1000000)

safeWrite :: IO a -> Smtpd a
safeWrite f = do
  to <- getWriteTimeout
  liftIO $ timeout to f >>= maybe (throwDyn (WriteTimeout to)) return

safeReply :: WriteHandle -> SmtpReply -> Smtpd ()
safeReply hOut r = safeWrite (hPutStr hOut (show r))

safeFlush :: WriteHandle -> Smtpd ()
safeFlush hOut = safeWrite (hFlush hOut)

-- |Like 'bracket', but only performs the final action if
-- there was an exception raised by the middle bit.

bracketOnError
	:: IO a		-- ^ computation to run first (\"acquire resource\")
	-> (a -> IO b)  -- ^ computation to run last (\"release resource\")
	-> (a -> IO c)	-- ^ computation to run in-between
	-> IO c		-- returns the value from the in-between computation
bracketOnError cons dest f =
  block $ do
    a <- cons
    catch (unblock (f a)) (\e -> dest a >> throw e)
