{-# LANGUAGE OverloadedStrings #-}

module Wikilon.WAI.Pages.DictWord.Rename
    ( dictWordRename
    , formDictWordRename
    ) where
    
import Control.Applicative
import Control.Monad
import Data.Monoid
import qualified Data.ByteString.Lazy as LBS
import qualified Network.HTTP.Types as HTTP
import Text.Blaze.Html5 ((!))
import qualified Text.Blaze.Html5 as H
import qualified Text.Blaze.Html5.Attributes as A
import qualified Network.Wai as Wai

import Wikilon.Time
import Wikilon.Dict.Word

import Wikilon.WAI.Utils
import Wikilon.WAI.Routes
import Wikilon.WAI.RecvFormPost
import qualified Wikilon.WAI.RegexPatterns as Regex

-- a simple form to rename a word.
formDictWordRename :: BranchName -> Word -> HTML
formDictWordRename d w =
    let uri = H.unsafeByteStringValue $ uriDictWordRename d w in
    let ws = wordToUTF8 w in
    H.form ! A.method "POST" ! A.action uri ! A.id "formDictWordRename" $ do
        let wv = H.unsafeByteStringValue ws 
        let rx = H.stringValue Regex.aoWord 
        H.input ! A.type_ "text" ! A.name "target" ! A.value wv ! A.pattern rx
        H.input ! A.type_ "submit" ! A.value "Rename"

dictWordRename :: WikilonApp
dictWordRename = app where
    app = routeOnMethod [(HTTP.methodGet, onGet), (HTTP.methodPost, onPost)]
    onGet = branchOnOutputMedia [(mediaTypeTextHTML, pageWordRename)]
    onPost = branchOnOutputMedia [(mediaTypeTextHTML, recvFormPost recvWordRename)]
    
-- a page with just the form to rename a word...
pageWordRename :: WikilonApp
pageWordRename = dictWordApp $ \ w dn dw _rq k ->
    let status = HTTP.ok200 in
    let headers = [textHtml] in
    let title = H.unsafeByteString $ "Rename " <> wordToUTF8 dw in
    k $ Wai.responseLBS status headers $ renderHTML $ do
        H.head $ do
            htmlHeaderCommon w
            H.title title
        H.body $ do
            formDictWordRename dn dw
            H.br
            renameMeta

renameMeta :: HTML
renameMeta = H.div ! A.class_ "docs" $ do
    H.p $ H.strong "Effects:" <> " after renaming, references to the original word\n\
          \are rewritten to the new target word, the original word is undefined,\n\
          \and the target word has the original's definition.\n\
          \"
    H.p $ H.strong "Limitations:" <> " to rename a word, the target word must\n\
          \either be undefined and unused, or have a byte-for-byte identical\n\
          \definition as the origin word, or one word must be a simple redirect\n\
          \to the other (i.e. `[{%foo}][]`). Rename does not modify behavior.\n\
          \"

recvWordRename :: PostParams -> WikilonApp
recvWordRename pp
  | (Just wt) <- Word . LBS.toStrict <$> getPostParam "target" pp
  , isValidWord wt
  = dictWordApp $ \ w dn wo _rq k ->
    let vc = vcache_space $ wikilon_store $ wikilon_model w in
    getTime >>= \ tNow ->
    join $ runVTx vc $ 
        let dicts = wikilon_dicts $ wikilon_model w in
        readPVar dicts >>= \ bset ->
        let b = Branch.lookup' dn bset in
        let d = Branch.head b in
        case Dict.renameWord wo wt d of
            Nothing -> 
                let status = HTTP.conflict409 in
                let headers = [textHtml, noCache] in
                let title = "Word Rename Conflict" in
                return $ k $ Wai.responseLBS status headers $ renderHTML $ do
                    H.head $ do
                        htmlMetaNoIndex
                        htmlHeaderCommon w
                        H.title title
                    H.body $ do
                        H.h1 title
                        H.p $ H.strong "origin: " <> " " <> hrefDictWord dn wo
                        H.p $ H.strong "target: " <> " " <> hrefDictWord dn wt
                        H.p $ "Rename failed. To succeed, the target word must be new,\n\
                              \identical to origin, or a simple redirect to/from origin.\n\
                              \No changes were made to the dictionary."
            Just d' -> do
                let b' = Branch.update (tNow, d') b 
                let bset' = Branch.insert dn b' bset 
                writePVar dicts bset' -- commit update
                markDurable -- push to disk
                -- prepare our response:
                let status = HTTP.seeOther303 
                let dest = (HTTP.hLocation, wikilon_httpRoot w <> uriDictWord dn wt) 
                let headers = [textHtml, noCache, dest] 
                let title = "Rename Successful" 
                return $ k $ Wai.responseLBS status headers $ renderHTML $ do
                    H.head $ do
                        htmlMetaNoIndex
                        htmlHeaderCommon w
                        H.title title
                    H.body $ do
                        H.h1 title
                        H.p $ "renamed to: " <> hrefDictWord dn wt 
recvWordRename _ = \ _w _cap _rq k -> 
    k $ eBadRequest $ "invalid target word"

