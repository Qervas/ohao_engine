#!/usr/bin/env python3
"""
AI Detection Analyzer for Technical Documentation

Checks text for common AI writing patterns using multiple detection methods:
1. Perplexity-style analysis (word predictability)
2. Burstiness (sentence length variation)
3. AI-typical phrases and patterns
4. Vocabulary diversity
5. Sentence structure analysis
"""

import re
import math
from collections import Counter
from typing import Dict, List, Tuple
import statistics


class AIDetector:
    """Multi-method AI detection analyzer"""

    # AI models love these phrases - humans rarely use them
    AI_PHRASES = [
        "it's important to note",
        "it's worth noting",
        "it is important to",
        "it is worth noting",
        "seamlessly",
        "robust",
        "leverage",
        "harness",
        "comprehensive",
        "delve into",
        "dive into",
        "in conclusion",
        "in summary",
        "to summarize",
        "as mentioned earlier",
        "as previously discussed",
        "it's crucial to",
        "it's essential to",
        "pivotal",
        "paramount",
        "facilitate",
        "utilize",
        "optimal",
        "streamline",
        "cutting-edge",
        "state-of-the-art",
        "revolutionize",
        "game-changing",
        "transformative",
        "empower",
        "unlock",
        "unlock the potential",
        "in today's",
        "in the realm of",
        "a myriad of",
        "plethora of",
        "multifaceted",
        "holistic",
        "synergy",
        "paradigm",
        "ecosystem",
        "landscape",
        "journey",
        "nuanced",
        "intricate",
        "sophisticated",
        "realm",
        "tapestry",
        "fabric of",
        "cornerstone",
        "foundation of",
        "testament to",
    ]

    # AI models tend to overuse these transition patterns
    AI_TRANSITIONS = [
        "however",
        "moreover",
        "furthermore",
        "additionally",
        "consequently",
        "therefore",
        "thus",
        "hence",
        "nevertheless",
        "nonetheless",
    ]

    # Common AI sentence starters
    AI_STARTERS = [
        "it's important",
        "it's worth",
        "it is important",
        "it is worth",
        "one of the",
        "when it comes to",
        "in order to",
        "the key to",
        "the importance of",
        "this is where",
        "this is what",
        "that being said",
        "with that said",
        "at the end of the day",
    ]

    def __init__(self, text: str):
        self.text = text
        self.sentences = self._split_sentences(text)
        self.words = self._extract_words(text)

    def _split_sentences(self, text: str) -> List[str]:
        """Split text into sentences"""
        # Remove code blocks first
        text = re.sub(r'```[\s\S]*?```', '', text)
        # Split on sentence endings
        sentences = re.split(r'[.!?]+', text)
        return [s.strip() for s in sentences if s.strip()]

    def _extract_words(self, text: str) -> List[str]:
        """Extract words, removing code and special chars"""
        # Remove code blocks
        text = re.sub(r'```[\s\S]*?```', '', text)
        # Extract words
        words = re.findall(r'\b[a-z]+\b', text.lower())
        return words

    def calculate_perplexity(self) -> float:
        """
        Calculate perplexity score (simplified version)
        Higher perplexity = more unpredictable = more human-like
        Lower perplexity = more predictable = more AI-like

        This is a simplified metric based on word frequency distribution
        Real perplexity requires a language model
        """
        if not self.words:
            return 0.0

        word_counts = Counter(self.words)
        total_words = len(self.words)

        # Calculate entropy (measure of unpredictability)
        entropy = 0.0
        for count in word_counts.values():
            probability = count / total_words
            entropy -= probability * math.log2(probability)

        # Convert to perplexity-like score
        perplexity = 2 ** entropy

        # Normalize to 0-100 scale
        # High perplexity (>100) = human-like
        # Low perplexity (<50) = AI-like
        normalized = min(100, perplexity * 10)

        return normalized

    def calculate_burstiness(self) -> float:
        """
        Calculate burstiness - variation in sentence length
        High burstiness = varied sentence lengths = more human
        Low burstiness = uniform sentence lengths = more AI
        """
        if len(self.sentences) < 2:
            return 0.0

        lengths = [len(s.split()) for s in self.sentences]

        if not lengths:
            return 0.0

        mean_length = statistics.mean(lengths)

        if mean_length == 0:
            return 0.0

        # Calculate coefficient of variation
        try:
            std_dev = statistics.stdev(lengths)
            cv = (std_dev / mean_length) * 100
        except:
            cv = 0.0

        # Higher CV = more variation = more human
        # Normalize: >50 is very human, <20 is very AI
        return min(100, cv * 2)

    def detect_ai_phrases(self) -> Tuple[int, List[str]]:
        """Count AI-typical phrases and return them"""
        text_lower = self.text.lower()
        found_phrases = []

        for phrase in self.AI_PHRASES:
            if phrase in text_lower:
                count = text_lower.count(phrase)
                found_phrases.extend([phrase] * count)

        return len(found_phrases), found_phrases

    def detect_ai_transitions(self) -> int:
        """Count overuse of formal transitions"""
        text_lower = self.text.lower()
        count = 0

        for transition in self.AI_TRANSITIONS:
            # Match as whole word
            pattern = r'\b' + re.escape(transition) + r'\b'
            count += len(re.findall(pattern, text_lower))

        return count

    def detect_ai_starters(self) -> int:
        """Count AI-typical sentence starters"""
        count = 0

        for sentence in self.sentences:
            sentence_lower = sentence.lower().strip()
            for starter in self.AI_STARTERS:
                if sentence_lower.startswith(starter):
                    count += 1
                    break

        return count

    def calculate_vocabulary_diversity(self) -> float:
        """
        Calculate lexical diversity (Type-Token Ratio)
        Higher diversity = more human-like
        AI tends to repeat words more
        """
        if not self.words:
            return 0.0

        unique_words = len(set(self.words))
        total_words = len(self.words)

        ttr = (unique_words / total_words) * 100
        return ttr

    def analyze_sentence_structure(self) -> Dict[str, float]:
        """Analyze sentence structure patterns"""
        if not self.sentences:
            return {}

        lengths = [len(s.split()) for s in self.sentences]

        # Count very short sentences (1-3 words) - humans use these
        short_sentences = sum(1 for l in lengths if l <= 3)
        short_ratio = (short_sentences / len(lengths)) * 100

        # Count very long sentences (>40 words) - AI sometimes does run-ons
        long_sentences = sum(1 for l in lengths if l > 40)
        long_ratio = (long_sentences / len(lengths)) * 100

        # Average sentence length
        avg_length = statistics.mean(lengths)

        return {
            'short_sentence_ratio': short_ratio,
            'long_sentence_ratio': long_ratio,
            'avg_sentence_length': avg_length,
        }

    def get_comprehensive_score(self) -> Dict[str, any]:
        """Run all detection methods and return comprehensive results"""

        perplexity = self.calculate_perplexity()
        burstiness = self.calculate_burstiness()
        ai_phrase_count, ai_phrases = self.detect_ai_phrases()
        ai_transition_count = self.detect_ai_transitions()
        ai_starter_count = self.detect_ai_starters()
        vocab_diversity = self.calculate_vocabulary_diversity()
        sentence_structure = self.analyze_sentence_structure()

        # Calculate overall AI probability (0-100, higher = more AI-like)
        ai_score = 0

        # Perplexity (lower = more AI)
        if perplexity < 30:
            ai_score += 30
        elif perplexity < 50:
            ai_score += 20
        elif perplexity < 70:
            ai_score += 10

        # Burstiness (lower = more AI)
        if burstiness < 20:
            ai_score += 25
        elif burstiness < 40:
            ai_score += 15
        elif burstiness < 60:
            ai_score += 5

        # AI phrases (per 1000 words)
        if self.words:
            phrases_per_1000 = (ai_phrase_count / len(self.words)) * 1000
            if phrases_per_1000 > 10:
                ai_score += 20
            elif phrases_per_1000 > 5:
                ai_score += 15
            elif phrases_per_1000 > 2:
                ai_score += 10

        # Transitions (per 100 sentences)
        if self.sentences:
            transitions_per_100 = (ai_transition_count / len(self.sentences)) * 100
            if transitions_per_100 > 20:
                ai_score += 15
            elif transitions_per_100 > 10:
                ai_score += 10
            elif transitions_per_100 > 5:
                ai_score += 5

        # Vocabulary diversity (lower = more AI)
        if vocab_diversity < 30:
            ai_score += 10
        elif vocab_diversity < 40:
            ai_score += 5

        # Normalize to 0-100
        ai_score = min(100, ai_score)

        # Human likelihood is inverse
        human_score = 100 - ai_score

        return {
            'ai_probability': ai_score,
            'human_probability': human_score,
            'verdict': 'Likely AI-generated' if ai_score > 70 else
                      'Possibly AI-generated' if ai_score > 50 else
                      'Possibly human-written' if ai_score > 30 else
                      'Likely human-written',
            'metrics': {
                'perplexity': round(perplexity, 2),
                'burstiness': round(burstiness, 2),
                'vocabulary_diversity': round(vocab_diversity, 2),
                'ai_phrase_count': ai_phrase_count,
                'ai_transition_count': ai_transition_count,
                'ai_starter_count': ai_starter_count,
                'total_words': len(self.words),
                'total_sentences': len(self.sentences),
            },
            'sentence_structure': sentence_structure,
            'found_ai_phrases': list(set(ai_phrases))[:10],  # Top 10 unique
        }


def analyze_file(filepath: str) -> None:
    """Analyze a file for AI detection"""
    print(f"\n{'='*80}")
    print(f"AI DETECTION ANALYSIS: {filepath}")
    print(f"{'='*80}\n")

    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            text = f.read()
    except Exception as e:
        print(f"Error reading file: {e}")
        return

    detector = AIDetector(text)
    results = detector.get_comprehensive_score()

    # Print results
    print(f"📊 OVERALL ASSESSMENT")
    print(f"{'-'*80}")
    print(f"AI Probability:    {results['ai_probability']:.1f}%")
    print(f"Human Probability: {results['human_probability']:.1f}%")
    print(f"Verdict:          {results['verdict']}")

    print(f"\n📈 DETECTION METRICS")
    print(f"{'-'*80}")
    metrics = results['metrics']
    print(f"Perplexity Score:        {metrics['perplexity']:.2f} (higher = more human)")
    print(f"Burstiness Score:        {metrics['burstiness']:.2f} (higher = more human)")
    print(f"Vocabulary Diversity:    {metrics['vocabulary_diversity']:.2f}% (higher = more human)")
    print(f"AI Phrases Found:        {metrics['ai_phrase_count']}")
    print(f"AI Transitions:          {metrics['ai_transition_count']}")
    print(f"AI Sentence Starters:    {metrics['ai_starter_count']}")

    print(f"\n📝 DOCUMENT STATS")
    print(f"{'-'*80}")
    print(f"Total Words:      {metrics['total_words']:,}")
    print(f"Total Sentences:  {metrics['total_sentences']:,}")

    struct = results['sentence_structure']
    print(f"\n🔤 SENTENCE STRUCTURE")
    print(f"{'-'*80}")
    print(f"Average Length:         {struct['avg_sentence_length']:.1f} words")
    print(f"Short Sentences (≤3):   {struct['short_sentence_ratio']:.1f}%")
    print(f"Long Sentences (>40):   {struct['long_sentence_ratio']:.1f}%")

    if results['found_ai_phrases']:
        print(f"\n⚠️  AI PHRASES DETECTED")
        print(f"{'-'*80}")
        for i, phrase in enumerate(results['found_ai_phrases'][:10], 1):
            print(f"{i}. '{phrase}'")

    print(f"\n💡 INTERPRETATION")
    print(f"{'-'*80}")

    if results['ai_probability'] < 30:
        print("✅ This text shows strong human characteristics:")
        print("   - Natural variation in sentence length")
        print("   - Conversational tone with personal touches")
        print("   - Limited use of AI-typical phrases")
        print("   - Good vocabulary diversity")
    elif results['ai_probability'] < 50:
        print("⚠️  This text shows some AI characteristics but could be human:")
        print("   - Some formal patterns detected")
        print("   - Consider adding more personality and variation")
    elif results['ai_probability'] < 70:
        print("🤖 This text likely has AI characteristics:")
        print("   - Consistent sentence structures")
        print("   - Multiple AI-typical phrases detected")
        print("   - Consider rewriting for more natural flow")
    else:
        print("🚨 This text strongly appears AI-generated:")
        print("   - High concentration of AI patterns")
        print("   - Formal transitions and predictable structure")
        print("   - Needs significant humanization")

    print(f"\n{'='*80}\n")


if __name__ == "__main__":
    import sys

    if len(sys.argv) > 1:
        filepath = sys.argv[1]
    else:
        # Default to the documentation file
        filepath = "ENGINE_DOCUMENTATION.md"

    analyze_file(filepath)

    print("\n💡 TIP: To make text more human-like:")
    print("   1. Add personal anecdotes and emotions ('I literally cheered')")
    print("   2. Use contractions (don't, can't, it's)")
    print("   3. Vary sentence length dramatically (short and long)")
    print("   4. Use informal language occasionally ('sucked', 'mess', 'nightmare')")
    print("   5. Remove phrases like 'it's important to note', 'leverage', 'robust'")
    print("   6. Start sentences differently (not always subject-verb)")
    print("   7. Add opinions and personality")
    print()
