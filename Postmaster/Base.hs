{-# OPTIONS -fglasgow-exts #-}
{- |
   Module      :  Postmaster.Base
   Copyright   :  (c) 2005-02-10 by Peter Simons
   License     :  GPL2

   Maintainer  :  simons@cryp.to
   Stability   :  provisional
   Portability :  Haskell 2-pre
 -}

module Postmaster.Base where

import Prelude hiding ( catch )
import Foreign
import System.IO
import Network.Socket hiding ( listen, shutdown )
import Control.Exception
import Control.Monad.RWS hiding ( local )
import MonadEnv
import BlockIO
import Data.Typeable
import Rfc2821 hiding ( path )

-- * The @Smtpd@ Monad

type SmtpdState = Env
type Smtpd a    = RWST GlobalEnv [LogMsg] SmtpdState IO a

-- |@say a b c msg = return ('reply' a b c [msg])@
--
-- The 'SmtpReply' codes returned the event handler returns
-- determine what Postmaster will do:
--
-- [@1xx@, @2xx@, @3xx@] make the 'SessionState' transition
-- determined determined by 'smtpdFSM'.
--
-- [@4xx@, @5xx@] Do /not/ make the transition.
--
-- [@221@, @421@] Drop the connection after this reply.
--
-- The reply for the 'Greeting' event (the first event
-- triggered when a session starts up) is interpreted as
-- follows: @2xx@ accepts the connection, everything else
-- refuses the connection.

say :: Int -> Int -> Int -> String -> Smtpd SmtpReply
say a b c msg = return (reply a b c [msg])

-- ** Environment

global :: EnvT a -> Smtpd a
global f = ask >>= liftIO . global' f

local :: EnvT a -> Smtpd a
local f = do
  (a, st) <- gets (runState f)
  put st
  return a

type SmtpdVariable = forall a. Typeable a => (Variable -> EnvT a) -> Smtpd a

defineLocal :: String -> SmtpdVariable
defineLocal n = \f -> local (f (mkVar n))

defineGlobal :: String -> SmtpdVariable
defineGlobal n = \f -> global (f (mkVar n))

type ID = Int

-- |Produce a unique 'ID' using a global counter.

getUniqueID :: Smtpd ID
getUniqueID = global $ tick (mkVar "UniqueID")

-- |Provides a unique 'ID' for this session.

mySessionID :: Smtpd ID
mySessionID = do
  let key = mkVar "SessionID"
  sid' <- local $ getval key
  case sid' of
    Just sid -> return sid
    _        -> do sid <- getUniqueID
                   local (setval key sid)
                   return sid

-- ** Event Handler

type EventHandler = Event -> Smtpd SmtpReply

type EventT  = EventHandler -> EventHandler

-- ** Data Handler

type DataHandler = Buffer -> Smtpd (Maybe SmtpReply, Buffer)


data LogMsg = LogMsg ID SmtpdState LogEvent
            deriving (Show)

-- |Given a log event, construct a 'LogMsg' and write it to
-- the monad's log stream with 'tell'. Every log event
-- contains the current 'SmtpdState' and the \"SessionID\".

yell :: LogEvent -> Smtpd ()
yell e = do
  sid <- mySessionID
  st <- get
  tell [LogMsg sid st e]

data LogEvent
  = Msg String
  | StartSession
  | EndSession
  | Input String
  | Output String
  | AcceptConn SockAddr
  | DropConn SockAddr
  | UserShutdown
  | CaughtException Exception
  | CaughtIOError IOException
  | StartEventHandler String Event
  | EventHandlerResult String Event SmtpCode
  | CurrentState
  | AssignMailID ID
  deriving (Show)

-- * Exception Handling

-- |Run a computation and fall back to the second if the
-- first throws an exception. The error is logged. An
-- exception in the second computation will propagate.

fallback
  :: Smtpd a                    -- ^ computation to run
  -> Smtpd a                    -- ^ fallback function
  -> Smtpd a
fallback f g = do
  cfg <- ask
  st <- get
  (r, st', w) <- liftIO $ catch
      (runRWST f cfg st)
      (\e -> runRWST (yell (CaughtException e) >> g) cfg st)
  tell w
  put st'
  return r
