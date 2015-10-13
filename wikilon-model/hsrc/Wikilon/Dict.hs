{-# LANGUAGE DeriveDataTypeable #-}
-- | Representation of AO dictionaries used in Wikilon.
-- 
-- For Wikilon, my goal is to host multiple dictionaries for DVCS
-- like forks, fast diffs for easy merges, a lot of shared structure
-- for efficient storage. Dictionaries can be very large, especially
-- due to dictionary applications... potentially millions of words 
-- and many gigabytes of definitions. So it's important that we can
-- load just the essential fragments of a dictionary into memory. 
-- 
-- To meet these goals, I'm currently using a VCache trie to model
-- the dictionary. Tries support structure sharing and fast diffs.
-- But separate indexes will be kept per branch for reverse lookup,
-- fuzzy find, and other features.
-- 
module Wikilon.Dict
    ( Dict
    , dictCreate
    , dictLookup
    , dictList
    , dictInsert
    , dictDelete
    , dictDiff, DictDiff, Diff(..)
    , dictTransitiveDepsList
    , module Wikilon.Word
    , module Wikilon.AODef
    ) where

import Control.Exception (assert)
import Control.Applicative
import Data.Typeable (Typeable)
import Data.Monoid
import qualified Data.List as L
import qualified Data.Set as Set
import qualified Data.ByteString as BS
import Data.VCache.Trie (Trie, Diff(..))
import qualified Data.VCache.Trie as Trie
import Database.VCache
import Wikilon.Word
import Wikilon.AODef

-- | An AO dictionary is a finite collection of (word, definition) 
-- pairs. In a healthy dictionary: definitions are valid AODef (a
-- subset of Awelon Bytecode (ABC)), dependencies between words are
-- acyclic, there are no dependencies on undefined words, and the
-- words are all well-typed. 
--
-- But Wikilon does permit representation of dictionaries that are
-- not healthy. It's left to higher layers to prevent or report 
-- any issues. Thus, users of the `Dict` type should not assume
-- the dictionary is (for example) acyclic.
-- 
newtype Dict = Dict (Trie Def)
    deriving (Eq, Typeable)

-- NOTE: I want to separate large definitions from the Trie nodes to avoid
-- copying them too often. But small definitions can be kept with the trie
-- nodes to reduce indirection. Here, 'small' will be up to 254 bytes.
--
-- Most definitions will probably be 'small', i.e. because 254 bytes is a
-- lot larger than a typical command line (even with view expansions). But
-- some definitions, especially embedded texts, will be much larger. 
data Def 
    = DefS AODef            -- for small definitions, < 255 bytes
    | DefL (VRef AODef)     -- for large definitions
    deriving (Eq, Typeable)

isSmall :: AODef -> Bool
isSmall = (< 255) . BS.length

aodef :: Def -> AODef
aodef (DefS def) = def
aodef (DefL ref) = deref' ref

toDef :: VSpace -> AODef -> Def
toDef vc def 
    | isSmall def = DefS def
    | otherwise   = DefL (vref' vc def)

-- | Create a new, empty dictionary.
dictCreate :: VSpace -> Dict
dictCreate = Dict . Trie.empty

-- | lookup a word in the dictionary. If a word is undefined,
-- this will return Nothing. Otherwise, it returns the bytecode
-- for the definition.
dictLookup :: Dict -> Word -> Maybe AODef
dictLookup (Dict t) (Word w) = aodef <$> Trie.lookup w t

-- | list all non-empty definitions in the dictionary.
dictList :: Dict -> [(Word, AODef)]
dictList (Dict t) = Trie.toListBy fn t where
    fn w d = (Word w, aodef d)

-- | List transitive dependencies for a list of root words. Each word
-- in the input list appears in the output list after all of its
-- dependencies. A word is listed in the output at most once.
dictTransitiveDepsList :: Dict -> [Word] -> [(Word, Maybe AODef)]
dictTransitiveDepsList dict = accum mempty mempty where
    -- accum (visited) (cycle prevention) (roots) 
    accum _ _ [] = []
    accum v c ws@(w:ws') =
        if Set.member w v then accum v c ws' else -- already listed w
        case dictLookup dict w of
            Nothing -> (w, Nothing) : accum (Set.insert w v) c ws'
            Just def -> 
                let lDeps = L.filter (`Set.notMember` v) (aodefWords def) in
                let bAddWord = L.null lDeps || Set.member w c in
                if bAddWord then (w, Just def) : accum (Set.insert w v) (Set.delete w c) ws'
                            else accum v (Set.insert w c) (lDeps ++ ws)

-- | insert a word into a dictionary. Note that this does not check
-- that the definition is sensible or that the resulting dictionary
-- is valid. 
dictInsert :: Dict -> Word -> AODef -> Dict
dictInsert (Dict t) (Word w) def = Dict $ Trie.insert w d t where
    d = toDef (Trie.trie_space t) def

-- | Delete a word from a dictionary. 
dictDelete :: Dict -> Word -> Dict
dictDelete (Dict t) (Word w) = Dict $ Trie.delete w t

-- | Quickly compute differences between two dictionaries.
dictDiff :: Dict -> Dict -> DictDiff
dictDiff (Dict a) (Dict b) = fmap toDictDiff $ Trie.diff a b where 
    toDictDiff (w, d) = (Word w, fmap aodef d) 

type DictDiff = [(Word, Diff AODef)]

instance VCacheable Dict where
    put (Dict d) = putWord8 1 >> put d
    get = getWord8 >>= \ v -> case v of
        1 -> Dict <$> get
        _ -> fail $ dictErr $ "unrecognized Dict version " ++ show v
instance VCacheable Def where
    put (DefL ref) = putWord8 255 >> put ref
    put (DefS def) = assert (isSmall def) $
        let sz = fromIntegral (BS.length def) in
        putWord8 sz >> putByteString def
    get = getWord8 >>= \ sz -> 
        if (255 == sz) then DefL <$> getVRef else
        DefS <$> getByteString (fromIntegral sz)

dictErr :: String -> String
dictErr = ("Wikilon.Dict " ++)
