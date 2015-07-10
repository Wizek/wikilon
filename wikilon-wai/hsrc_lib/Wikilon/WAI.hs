{-# LANGUAGE OverloadedStrings #-}

-- | Web Application Interface bindings for Wikilon. 
--
-- Using Network.Wai.Route to easily cover common cases.
module Wikilon.WAI 
    ( wikilonWaiConf
    , wikilonWaiApp
    , wikilonRoutes
    ) where

import Control.Arrow (first)
import Control.Exception (catch)
import Data.Monoid
import Data.ByteString (ByteString)
import qualified Data.ByteString as BS
import qualified Data.ByteString.UTF8 as UTF8
import qualified Data.ByteString.Lazy.UTF8 as LazyUTF8
import qualified Network.HTTP.Types as HTTP
import qualified Network.Wai as Wai
import qualified Network.Wai.Route.Tree as Tree
import Wikilon.WAI.Types
import Wikilon.WAI.Routes
import Wikilon.WAI.Pages
import Wikilon.WAI.DefaultCSS
import Wikilon.WAI.Utils (plainText, noCache, eServerError, eNotFound)

import Wikilon.Store.Branch (BranchName)
import Wikilon.Store.Root

-- | List of routes for the Wikilon web service.
wikilonRoutes :: [(Route, WikilonApp)]
wikilonRoutes = fmap (first UTF8.fromString) $ 
    [("/", wikilonRoot)
    ,("/d", allDictionaries)
    ,("/d/:d", dictResource)
    ,("/d/:d/aodict", dictAsAODict)
    ,("/d/:d/aodict.edit", appAODictEdit)

    ,("/d/:d/w", dictWords)
    ,("/d/:d/w/:w", dictWord)
    ,("/d/:d/w/:w/rename", dictWordRename)
    ,("/d/:d/w/:w/abc", dictWordCompile)
    ,("/d/:d/w/:w/aodef", dictWordAODef)
    ,("/d/:d/w/:w/aodef.edit", dictWordAODefEdit)
    ,("/d/:d/w/:w/clawdef", dictWordClawDef)
    ,("/d/:d/w/:w/clawdef.edit", dictWordClawDefEdit)

    ,("/d.create", dictCreate)
    --,("/d/:d/hist", dictHist)

    -- thoughts:
    --  I could use /d/:d/name and /d/:d/w/:w/name 
    --  as handles for renaming
    
--    ,("/u",listOfUsers)
--    ,("/u/:u",singleUser)


    -- administrative
    ,("/admin/dbHealth", dbHealth)

    -- built-in documentation
    ,("/about/aodict", aodictDocs)
    ,("/about/claw", clawDocs)

    -- special endpoints to force media types
    ,("/d.list", dictList)
    ,("/d/:d/w.list", dictWordsList)

    -- generic endpoints
--    ,("/favicon", resourceFavicon)
    ,("/css", resourceDefaultCSS)
    ]

wikilonWaiConf :: ByteString -> BranchName -> WikilonStore -> Wikilon
wikilonWaiConf _httpRoot _master _model = Wikilon
    { wikilon_httpRoot = wrapSlash _httpRoot
    , wikilon_master = _master
    , wikilon_model = _model
    }

-- | The primary wikilon web service. Any exceptions will be logged
-- for administrators and return a 500 response.
wikilonWaiApp :: Wikilon -> Wai.Application
wikilonWaiApp w rq k = catch (baseWikilonApp w rq k) $ \ e -> do 
    logSomeException (wikilon_model w) e
    k $ eServerError "unhandled exception (logged for admin)"

baseWikilonApp :: Wikilon -> Wai.Application
baseWikilonApp w rq k =
    -- require: request is received under Wikilon's httpRoot
    let nRootLen = BS.length (wikilon_httpRoot w) in
    let (uriRoot,uriPath) = BS.splitAt nRootLen (Wai.rawPathInfo rq) in
    let badPath = k $ eServerError "inconsistent path" in
    if uriRoot /= wikilon_httpRoot w then badPath else
    -- otherwise handle the request normally
    let t = Tree.fromList wikilonRoutes in
    let s = Tree.segments uriPath in
    case Tree.lookup t s of
        -- most apps will use Network.Wai.Route
        Just route -> app w cap rq k where
            app = Tree.value route
            cap = Tree.captured $ Tree.captures route
        -- ad-hoc special cases
        Nothing -> case s of
            ("d":dictPath:"wiki":_) -> remaster dictPath w rq k
            ("dev":"echo":_) -> echo rq k
            _ -> k $ eNotFound rq 

-- | allow 'views' of Wikilon using a different master dictionary.
--
-- \/d\/foo\/wiki\/  -  view wikilon via the 'foo' dictionary
--
-- While a single dictionary is the default master for Wikilon, it
-- is always possible to treat other dictionaries as the new master.
-- This does add some access costs, but those should be marginal.
--
remaster :: BranchName -> Wikilon -> Wai.Application
remaster dictPath w rq k =
    -- "d/fooPath/wiki/" adds 8 + length "fooPath" to old prefix. 
    -- We must urlDecode "fooPath" to recover the dictionary name.
    let prefixLen = 8 + BS.length dictPath + BS.length (wikilon_httpRoot w) in
    let _httpRoot = BS.take prefixLen (Wai.rawPathInfo rq) in
    let _master = HTTP.urlDecode False dictPath in
    let w' = w { wikilon_master = _master, wikilon_httpRoot = _httpRoot } in
    baseWikilonApp w' rq k

-- | Echo request (for development purposes)
echo :: Wai.Application
echo rq k = k response where
    response = Wai.responseLBS HTTP.ok200 [plainText,noCache] body
    body = LazyUTF8.fromString $ show rq

-- initial root will always start and end with '/'. The
-- empty string is modified to just "/". This simplifies
-- construction of 'base' and alternative masters.
wrapSlash :: ByteString -> ByteString
wrapSlash = finiSlash . initSlash

finiSlash :: ByteString -> ByteString
finiSlash s = case BS.unsnoc s of
    Just (_, 47) -> s
    _ -> s <> "/"

initSlash :: ByteString -> ByteString
initSlash s = case BS.uncons s of
    Just (47, _) -> s
    _ -> "/" <> s



